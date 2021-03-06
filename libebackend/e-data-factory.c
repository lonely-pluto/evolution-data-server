/*
 * e-data-factory.c
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * SECTION: e-data-factory
 * @include: libebackend/libebackend.h
 * @short_description: An abstract base class for a backend-based server
 **/

#include "e-data-factory.h"

#include <config.h>
#include <glib/gi18n-lib.h>

#include <libedataserver/libedataserver.h>

#include <libebackend/e-backend-factory.h>
#include <libebackend/e-dbus-server.h>

#include <e-dbus-subprocess-backend.h>

#define E_DATA_FACTORY_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_DATA_FACTORY, EDataFactoryPrivate))

typedef enum {
	DATA_FACTORY_SPAWN_SUBPROCESS_NONE = 0,
	DATA_FACTORY_SPAWN_SUBPROCESS_BLOCKED,
	DATA_FACTORY_SPAWN_SUBPROCESS_READY
} DataFactorySpawnSubprocessStates;

struct _EDataFactoryPrivate {
	ESourceRegistry *registry;

	/* The mutex guards the 'subprocess_helpers' hash table.
	 * The 'backend_factories' hash table doesn't really need
	 * guarding since it gets populated during construction
	 * and is read-only thereafter. */
	GMutex mutex;

	/* Factory Name -> DataFactorySubprocessFactoryHelper */
	GHashTable *subprocess_helpers;

	/* Bus Name -> Watched Name */
	GHashTable *subprocess_watched_ids;
	GMutex subprocess_watched_ids_lock;

	/* Hash Key -> EBackendFactory */
	GHashTable *backend_factories;

	/* This is a hash table of client bus names to an array of
	 * EDBusSubprocessBackend references; one for every connection
	 * opened. */
	GHashTable *connections;
	GRecMutex connections_lock;

	/* This is a hash table of client bus names being watched.
	 * The value is the watcher ID for g_bus_unwatch_name(). */
	GHashTable *watched_names;
	GMutex watched_names_lock;

	/* This is a GCond used to guarantee that in case of two
	 * instances of the same backend are created at the same
	 * time, the second instance will wait the first one be
	 * created and then will use that instance, instead of
	 * creating it twice. */
	GCond spawn_subprocess_cond;
	GMutex spawn_subprocess_lock;
	DataFactorySpawnSubprocessStates spawn_subprocess_state;
};

enum {
	PROP_0,
	PROP_REGISTRY
};

/* Forward Declarations */
static void	e_data_factory_initable_init	(GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (
	EDataFactory,
	e_data_factory,
	E_TYPE_DBUS_SERVER,
	G_IMPLEMENT_INTERFACE (
		G_TYPE_INITABLE,
		e_data_factory_initable_init))

typedef struct _DataFactorySpawnSubprocessBackendThreadData DataFactorySpawnSubprocessBackendThreadData;

struct _DataFactorySpawnSubprocessBackendThreadData {
	EDataFactory *data_factory;
	GDBusMethodInvocation *invocation;
	gchar *uid;
	gchar *extension_name;
	gchar *subprocess_path;
};

static DataFactorySpawnSubprocessBackendThreadData *
data_factory_spawn_subprocess_backend_thread_data_new (EDataFactory *data_factory,
						       GDBusMethodInvocation *invocation,
						       const gchar *uid,
						       const gchar *extension_name,
						       const gchar *subprocess_path)
{
	DataFactorySpawnSubprocessBackendThreadData *data;

	data = g_new0 (DataFactorySpawnSubprocessBackendThreadData, 1);
	data->data_factory = g_object_ref (data_factory);
	data->invocation = g_object_ref (invocation);
	data->uid = g_strdup (uid);
	data->extension_name = g_strdup (extension_name);
	data->subprocess_path = g_strdup (subprocess_path);

	return data;
}

static void
data_factory_spawn_subprocess_backend_thread_data_free (DataFactorySpawnSubprocessBackendThreadData *data)
{
	if (data != NULL) {
		g_clear_object (&data->data_factory);
		g_clear_object (&data->invocation);
		g_free (data->uid);
		g_free (data->extension_name);
		g_free (data->subprocess_path);

		g_free (data);
	}
}

typedef struct _DataFactorySubprocessHelper DataFactorySubprocessHelper;

struct _DataFactorySubprocessHelper {
	EDBusSubprocessBackend *proxy;
	gchar *bus_name;
};

static DataFactorySubprocessHelper *
data_factory_subprocess_helper_new (EDBusSubprocessBackend *proxy,
				    const gchar *bus_name)
{
	DataFactorySubprocessHelper *helper;

	helper = g_new0 (DataFactorySubprocessHelper, 1);
	helper->proxy = g_object_ref (proxy);
	helper->bus_name = g_strdup (bus_name);

	return helper;
}

static void
data_factory_subprocess_helper_free (DataFactorySubprocessHelper *helper)
{
	if (helper != NULL) {
		g_clear_object (&helper->proxy);
		g_free (helper->bus_name);

		g_free (helper);
	}
}

static gchar *
data_factory_construct_subprocess_path (EDataFactory *data_factory)
{
	EDataFactoryClass *class;
	static volatile gint counter = 1;

	g_return_val_if_fail (E_IS_DATA_FACTORY (data_factory), NULL);

	g_atomic_int_inc (&counter);

	class = E_DATA_FACTORY_GET_CLASS (data_factory);
	g_return_val_if_fail (class->subprocess_object_path_prefix != NULL, NULL);

	return g_strdup_printf (
		"%s/%d/%u",
		class->subprocess_object_path_prefix, getpid (), counter);
}

static gchar *
data_factory_construct_subprocess_bus_name (EDataFactory *data_factory)
{
	EDataFactoryClass *class;
	static volatile gint counter = 1;

	g_return_val_if_fail (E_IS_DATA_FACTORY (data_factory), NULL);

	g_atomic_int_inc (&counter);

	class = E_DATA_FACTORY_GET_CLASS (data_factory);
	g_return_val_if_fail (class->subprocess_bus_name_prefix != NULL, NULL);

	/* We use the format "%sx%d%u" because we want to be really safe about
	 * the GDBus bus names used. When we create an object path we follow
	 * the same pattern %s/%d/%u, but bus names are quite more restrictive
	 * about its format, not allowing ".", "-", "/" or whatever that could
	 * be used in a more descriptive way. And that's the reason the "x" is
	 * being used here. */
	return g_strdup_printf (
		"%sx%dx%u",
		class->subprocess_bus_name_prefix, getpid(), counter);
}

typedef struct _DataFactorySubprocessData DataFactorySubprocessData;

struct _DataFactorySubprocessData {
	EDataFactory *data_factory;
	GDBusMethodInvocation *invocation;
	gchar *bus_name;
	gchar *extension_name;
	gchar *factory_name;
	gchar *path;
	gchar *type_name;
	gchar *uid;
	gchar *module_filename;
	gchar *subprocess_helpers_hash_key;
};

static DataFactorySubprocessData *
data_factory_subprocess_data_new (EDataFactory *data_factory,
				  GDBusMethodInvocation *invocation,
				  const gchar *uid,
				  const gchar *factory_name,
				  const gchar *type_name,
				  const gchar *extension_name,
				  const gchar *module_filename,
				  const gchar *subprocess_helpers_hash_key)
{
	DataFactorySubprocessData *sd;

	sd = g_new0 (DataFactorySubprocessData, 1);
	sd->data_factory = g_object_ref (data_factory);
	sd->invocation = g_object_ref (invocation);
	sd->uid = g_strdup (uid);
	sd->factory_name = g_strdup (factory_name);
	sd->type_name = g_strdup (type_name);
	sd->extension_name = g_strdup (extension_name);
	sd->module_filename = g_strdup (module_filename);
	sd->subprocess_helpers_hash_key = g_strdup (subprocess_helpers_hash_key);
	sd->path = data_factory_construct_subprocess_path (data_factory);
	sd->bus_name = data_factory_construct_subprocess_bus_name (data_factory);

	return sd;
}

static void
data_factory_subprocess_data_free (DataFactorySubprocessData *sd)
{
	if (sd != NULL) {
		g_clear_object (&sd->data_factory);
		g_clear_object (&sd->invocation);
		g_free (sd->uid);
		g_free (sd->path);
		g_free (sd->bus_name);
		g_free (sd->type_name);
		g_free (sd->factory_name);
		g_free (sd->extension_name);
		g_free (sd->module_filename);
		g_free (sd->subprocess_helpers_hash_key);

		g_free (sd);
	}
}

static gchar *
data_factory_dup_subprocess_helper_hash_key (const gchar *factory_name,
					     const gchar *extension_name,
					     const gchar *uid,
					     gboolean backend_factory_share_subprocess)
{
	gchar *helper_hash_key;

#ifdef ENABLE_BACKEND_PER_PROCESS
	helper_hash_key = backend_factory_share_subprocess ?
		g_strdup (factory_name) : g_strdup_printf ("%s:%s:%s", factory_name, extension_name, uid);
#else
	helper_hash_key = g_strdup ("not-using-bacend-per-process");
#endif

	return helper_hash_key;
}

static gboolean
data_factory_verify_subprocess_backend_proxy_is_used (EDataFactory *data_factory,
						      const gchar *except_bus_name,
						      EDBusSubprocessBackend *proxy)
{
	GHashTable *connections;
	GList *names, *l;
	GPtrArray *array;
	gboolean is_used = FALSE;

	g_return_val_if_fail (except_bus_name != NULL, TRUE);

	g_rec_mutex_lock (&data_factory->priv->connections_lock);

	connections = data_factory->priv->connections;
	names = g_hash_table_get_keys (connections);

	for (l = names; l != NULL && !is_used; l = g_list_next (l)) {
		const gchar *client_bus_name = l->data;
		gint ii;

		/* Check if we don't have more connections from the same client */
		if (g_strcmp0 (client_bus_name, except_bus_name) == 0) {
			guint used = 0;
			array = g_hash_table_lookup (connections, client_bus_name);

			for (ii = 0; ii < array->len; ii++) {
				EDBusSubprocessBackend *proxy_in_use;

				proxy_in_use = g_ptr_array_index (array, ii);

				if (proxy == proxy_in_use)
					used++;

				if (used > 1) {
					is_used = TRUE;
					break;
				}
			}
			continue;
		}

		array = g_hash_table_lookup (connections, client_bus_name);
		for (ii = 0; ii < array->len; ii++) {
			EDBusSubprocessBackend *proxy_in_use;

			proxy_in_use = g_ptr_array_index (array, ii);

			if (proxy == proxy_in_use) {
				is_used = TRUE;
				break;
			}
		}
	}

	g_list_free (names);
	g_rec_mutex_unlock (&data_factory->priv->connections_lock);

	return is_used;
}

static gboolean
data_factory_connections_remove (EDataFactory *data_factory,
				 const gchar *name,
				 EDBusSubprocessBackend *proxy)
{
	GHashTable *connections;
	GPtrArray *array;
	gboolean removed = FALSE;

	/* If proxy is NULL, we remove all proxies for name. */
	g_return_val_if_fail (name != NULL, FALSE);

	g_rec_mutex_lock (&data_factory->priv->connections_lock);

	connections = data_factory->priv->connections;
	array = g_hash_table_lookup (connections, name);

	if (array != NULL) {
		if (proxy != NULL) {
			if (!data_factory_verify_subprocess_backend_proxy_is_used (data_factory, name, proxy))
				e_dbus_subprocess_backend_call_close_sync (proxy, NULL, NULL);

			removed = g_ptr_array_remove_fast (array, proxy);
		} else if (array->len > 0) {
			/*
			 * As we can have more than one proxy being used by the same name,
			 * in the same array, we must remove the connections one by one
			 * and then notify the subprocess to quit itself when it's the last
			 * one being used. AKA: do *not* try to optimize this code to use
			 * g_ptr_array_set_size (array, 0).
			 */
			while (array->len != 0) {
				EDBusSubprocessBackend *proxy1;

				proxy1 = g_ptr_array_index (array, 0);

				if (!data_factory_verify_subprocess_backend_proxy_is_used (data_factory, name, proxy1))
					e_dbus_subprocess_backend_call_close_sync (proxy1, NULL, NULL);

				g_ptr_array_remove_fast (array, proxy1);
			}

			removed = TRUE;
		}

		if (array->len == 0)
			g_hash_table_remove (connections, name);

		if (g_hash_table_size (connections) == 0)
			e_dbus_server_release (E_DBUS_SERVER (data_factory));
	}

	g_rec_mutex_unlock (&data_factory->priv->connections_lock);

	return removed;
}

static void
data_factory_name_vanished_cb (GDBusConnection *connection,
			       const gchar *name,
			       gpointer user_data)
{
	GWeakRef *weak_ref = user_data;
	EDataFactory *data_factory;

	data_factory = g_weak_ref_get (weak_ref);

	if (data_factory != NULL) {
		data_factory_connections_remove (data_factory, name, NULL);

		/* Unwatching the bus name from here will corrupt the
		 * 'name' argument, and possibly also the 'user_data'.
		 *
		 * This is a GDBus bug.  Work around it by unwatching
		 * the bus name last.
		 *
		 * See: https://bugzilla.gnome.org/706088
		 */
		g_mutex_lock (&data_factory->priv->watched_names_lock);
		g_hash_table_remove (data_factory->priv->watched_names, name);
		g_mutex_unlock (&data_factory->priv->watched_names_lock);

		g_object_unref (data_factory);
	}
}

static void
data_factory_watched_names_add (EDataFactory *data_factory,
				GDBusConnection *connection,
				const gchar *name)
{
	GHashTable *watched_names;

	g_return_if_fail (name != NULL);

	g_mutex_lock (&data_factory->priv->watched_names_lock);

	watched_names = data_factory->priv->watched_names;

	if (!g_hash_table_contains (watched_names, name)) {
		guint watcher_id;

		/* The g_bus_watch_name() documentation says one of the two
		 * callbacks are guaranteed to be invoked after calling the
		 * function.  But which one is determined asynchronously so
		 * there should be no chance of the name vanished callback
		 * deadlocking with us when it tries to acquire the lock. */
		watcher_id = g_bus_watch_name_on_connection (
			connection, name,
			G_BUS_NAME_WATCHER_FLAGS_NONE,
			(GBusNameAppearedCallback) NULL,
			data_factory_name_vanished_cb,
			e_weak_ref_new (data_factory),
			(GDestroyNotify) e_weak_ref_free);

		g_hash_table_insert (
			watched_names, g_strdup (name),
			GUINT_TO_POINTER (watcher_id));
	}

	g_mutex_unlock (&data_factory->priv->watched_names_lock);
}

static void
data_factory_connections_add (EDataFactory *data_factory,
			      const gchar *name,
			      EDBusSubprocessBackend *proxy)
{
	GHashTable *connections;
	GPtrArray *array;

	g_return_if_fail (name != NULL);
	g_return_if_fail (proxy != NULL);

	g_rec_mutex_lock (&data_factory->priv->connections_lock);

	connections = data_factory->priv->connections;

	array = g_hash_table_lookup (connections, name);

	if (array == NULL) {
		array = g_ptr_array_new_with_free_func (
			(GDestroyNotify) g_object_unref);
		g_hash_table_insert (
			connections, g_strdup (name), array);
	}

	g_ptr_array_add (array, g_object_ref (proxy));

	g_rec_mutex_unlock (&data_factory->priv->connections_lock);
}

static void
data_factory_call_subprocess_backend_create_sync (EDataFactory *data_factory,
						  EDBusSubprocessBackend *proxy,
						  GDBusMethodInvocation *invocation,
						  const gchar *uid,
						  const gchar *bus_name,
						  const gchar *type_name,
						  const gchar *extension_name,
						  const gchar *module_filename)
{
	GError *error = NULL;
	gchar *object_path = NULL;

	e_dbus_subprocess_backend_call_create_sync (
		proxy, uid, type_name, module_filename, &object_path, NULL, &error);

	if (object_path != NULL) {
		EDataFactoryClass *class;
		GDBusConnection *connection;
		const gchar *sender;

		connection = g_dbus_method_invocation_get_connection (invocation);
		sender = g_dbus_method_invocation_get_sender (invocation);

		data_factory_watched_names_add (
			data_factory, connection, sender);

		data_factory_connections_add (
			data_factory, sender, proxy);

		class = E_DATA_FACTORY_GET_CLASS (data_factory);
		g_return_if_fail (class->complete_open != NULL);

		class->complete_open (data_factory, invocation, object_path, bus_name, extension_name);

		g_free (object_path);
	} else {
		g_rec_mutex_lock (&data_factory->priv->connections_lock);
		if (g_hash_table_size (data_factory->priv->connections) == 0)
			e_dbus_server_release (E_DBUS_SERVER (data_factory));
		g_rec_mutex_unlock (&data_factory->priv->connections_lock);

		g_return_if_fail (error != NULL);
		g_dbus_method_invocation_take_error (invocation, error);
	}

	g_mutex_lock (&data_factory->priv->spawn_subprocess_lock);
	if (data_factory->priv->spawn_subprocess_state == DATA_FACTORY_SPAWN_SUBPROCESS_BLOCKED)
		data_factory->priv->spawn_subprocess_state = DATA_FACTORY_SPAWN_SUBPROCESS_NONE;
	g_cond_signal (&data_factory->priv->spawn_subprocess_cond);
	g_mutex_unlock (&data_factory->priv->spawn_subprocess_lock);
}

static void
data_factory_backend_closed_cb (EDBusSubprocessBackend *proxy,
				const gchar *sender,
				GWeakRef *factory_weak_ref)
{
	EDataFactory *data_factory;

	data_factory = g_weak_ref_get (factory_weak_ref);
	data_factory_connections_remove (data_factory, sender, proxy);
	g_object_unref (data_factory);
}

static void
data_factory_subprocess_appeared_cb (GDBusConnection *connection,
				     const gchar *name,
				     const gchar *name_owner,
				     gpointer user_data)
{
	EDBusSubprocessBackend *proxy;
	EDataFactoryPrivate *priv;
	DataFactorySubprocessData *sd;
	DataFactorySubprocessHelper *helper;

	sd = user_data;

	proxy = e_dbus_subprocess_backend_proxy_new_sync (
		connection,
		G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
		sd->bus_name,
		sd->path, NULL, NULL);

	g_signal_connect_data (
		proxy, "backend-closed",
		G_CALLBACK (data_factory_backend_closed_cb),
		e_weak_ref_new (sd->data_factory),
		(GClosureNotify) e_weak_ref_free,
		0);

	data_factory_call_subprocess_backend_create_sync (
		sd->data_factory,
		proxy,
		sd->invocation,
		sd->uid,
		sd->bus_name,
		sd->type_name,
		sd->extension_name,
		sd->module_filename);

	helper = data_factory_subprocess_helper_new (proxy, sd->bus_name);

	priv = sd->data_factory->priv;
	g_mutex_lock (&priv->mutex);
	g_hash_table_insert (
		priv->subprocess_helpers,
		g_strdup (sd->subprocess_helpers_hash_key),
		helper);
	g_mutex_unlock (&priv->mutex);

	g_mutex_lock (&priv->spawn_subprocess_lock);
	priv->spawn_subprocess_state = DATA_FACTORY_SPAWN_SUBPROCESS_READY;
	g_cond_signal (&priv->spawn_subprocess_cond);
	g_mutex_unlock (&priv->spawn_subprocess_lock);

	g_object_unref (proxy);
}

static void
data_factory_subprocess_vanished_cb (GDBusConnection *connection,
				     const gchar *name,
				     gpointer user_data)
{
	DataFactorySubprocessData *sd;
	DataFactorySubprocessHelper *helper;
	EDataFactoryPrivate *priv;
	const gchar *sender;
	guint watched_id;

	sd = user_data;
	priv = sd->data_factory->priv;

	g_mutex_lock (&priv->mutex);
	helper = g_hash_table_lookup (
		priv->subprocess_helpers,
		sd->subprocess_helpers_hash_key);
	g_mutex_unlock (&priv->mutex);

	if (helper == NULL)
		return;

	sender = g_dbus_method_invocation_get_sender (sd->invocation);
	data_factory_connections_remove (sd->data_factory, sender, helper->proxy);

	g_mutex_lock (&priv->mutex);
	g_hash_table_remove (
		priv->subprocess_helpers,
		sd->subprocess_helpers_hash_key);
	g_mutex_unlock (&priv->mutex);

	g_mutex_lock (&priv->subprocess_watched_ids_lock);
	watched_id = GPOINTER_TO_UINT (g_hash_table_lookup (priv->subprocess_watched_ids, sd->bus_name));
	g_hash_table_remove (priv->subprocess_watched_ids, sd->bus_name);

	if (watched_id > 0)
		g_bus_unwatch_name (watched_id);
	g_mutex_unlock (&priv->subprocess_watched_ids_lock);
}

static void
watched_names_value_free (gpointer value)
{
	g_bus_unwatch_name (GPOINTER_TO_UINT (value));
}

static void
data_factory_bus_acquired (EDBusServer *server,
			   GDBusConnection *connection)
{
	GDBusInterfaceSkeleton *skeleton_interface;
	EDataFactoryClass *class;
	GError *error = NULL;

	class = E_DATA_FACTORY_GET_CLASS (E_DATA_FACTORY (server));

	skeleton_interface = class->get_dbus_interface_skeleton (server);

	g_dbus_interface_skeleton_export (
		skeleton_interface,
		connection,
		class->factory_object_path,
		&error);

	if (error != NULL) {
		g_warning ("%s: %s", G_STRFUNC, error->message);
		e_dbus_server_quit (server, E_DBUS_SERVER_EXIT_NORMAL);
		g_error_free (error);

		return;
	}

	/* Chain up to parent's bus_acquired() method. */
	E_DBUS_SERVER_CLASS (e_data_factory_parent_class)->
		bus_acquired (server, connection);
}

static GList *
data_factory_list_proxies (EDataFactory *data_factory)
{
	GList *proxies = NULL;
	GHashTable *connections;
	GHashTable *proxies_hash;
	GHashTableIter iter;
	gpointer key, value;

	g_return_val_if_fail (E_IS_DATA_FACTORY (data_factory), NULL);

	g_rec_mutex_lock (&data_factory->priv->connections_lock);

	connections = data_factory->priv->connections;
	proxies_hash = g_hash_table_new (g_direct_hash, g_direct_equal);

	g_hash_table_iter_init (&iter, connections);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GPtrArray *array = value;
		gint ii;

		for (ii = 0; ii < array->len; ii++) {
			EDBusSubprocessBackend *proxy = g_ptr_array_index (array, ii);
			if (!g_hash_table_contains (proxies_hash, proxy)) {
				g_hash_table_insert (proxies_hash, proxy, GINT_TO_POINTER (1));
				proxies = g_list_prepend (proxies, g_object_ref (proxy));
			}
		}
	}

	g_hash_table_destroy (proxies_hash);
	proxies = g_list_reverse (proxies);
	g_rec_mutex_unlock (&data_factory->priv->connections_lock);

	return proxies;
}

static void
data_factory_connections_remove_all (EDataFactory *data_factory)
{
	GHashTable *connections;

	g_rec_mutex_lock (&data_factory->priv->connections_lock);

	connections = data_factory->priv->connections;

	if (g_hash_table_size (connections) > 0) {
		GList *proxies, *l;
		proxies = data_factory_list_proxies (data_factory);

		for (l = proxies; l != NULL; l = g_list_next (l)) {
			EDBusSubprocessBackend *proxy = l->data;
			e_dbus_subprocess_backend_call_close_sync (proxy, NULL, NULL);
		}

		g_list_free_full (proxies, g_object_unref);

		g_hash_table_remove_all (connections);
		e_dbus_server_release (E_DBUS_SERVER (data_factory));
	}

	g_rec_mutex_unlock (&data_factory->priv->connections_lock);
}

static void
data_factory_bus_name_lost (EDBusServer *server,
			    GDBusConnection *connection)
{
	EDataFactory *data_factory;

	data_factory = E_DATA_FACTORY (server);

	data_factory_connections_remove_all (data_factory);

	/* Chain up to parent's bus_name_lost() method. */
	E_DBUS_SERVER_CLASS (e_data_factory_parent_class)->
		bus_name_lost (server, connection);
}

static void
data_factory_quit_server (EDBusServer *server,
			  EDBusServerExitCode exit_code)
{
	GDBusInterfaceSkeleton *skeleton_interface;
	EDataFactoryClass *class;

	class = E_DATA_FACTORY_GET_CLASS (E_DATA_FACTORY (server));

	skeleton_interface = class->get_dbus_interface_skeleton (server);
	g_dbus_interface_skeleton_unexport (skeleton_interface);

	/* This factory does not support reloading, so stop the signal
	 * emission and return without chaining up to prevent quitting. */
	if (exit_code == E_DBUS_SERVER_EXIT_RELOAD) {
		g_signal_stop_emission_by_name (server, "quit-server");
		return;
	}

	/* Chain up to parent's quit_server() method. */
	E_DBUS_SERVER_CLASS (e_data_factory_parent_class)->
		quit_server (server, exit_code);
}

static void
e_data_factory_get_property (GObject *object,
			     guint property_id,
			     GValue *value,
			     GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_REGISTRY:
			g_value_set_object (
				value,
				e_data_factory_get_registry (
				E_DATA_FACTORY (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
data_factory_dispose (GObject *object)
{
	EDataFactory *data_factory;
	EDataFactoryPrivate *priv;

	data_factory = E_DATA_FACTORY (object);
	priv = data_factory->priv;

	g_hash_table_remove_all (priv->backend_factories);
	g_hash_table_remove_all (priv->subprocess_helpers);
	g_hash_table_remove_all (priv->subprocess_watched_ids);

	g_clear_object (&priv->registry);

	g_hash_table_remove_all (priv->connections);
	g_hash_table_remove_all (priv->watched_names);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_data_factory_parent_class)->dispose (object);
}

static void
data_factory_finalize (GObject *object)
{
	EDataFactory *data_factory;
	EDataFactoryPrivate *priv;

	data_factory = E_DATA_FACTORY (object);
	priv = data_factory->priv;

	g_mutex_clear (&priv->mutex);

	g_hash_table_destroy (priv->backend_factories);
	g_hash_table_destroy (priv->subprocess_helpers);

	g_hash_table_destroy (priv->subprocess_watched_ids);
	g_mutex_clear (&priv->subprocess_watched_ids_lock);

	g_hash_table_destroy (priv->connections);
	g_rec_mutex_clear (&priv->connections_lock);

	g_hash_table_destroy (priv->watched_names);
	g_mutex_clear (&priv->watched_names_lock);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_data_factory_parent_class)->finalize (object);
}

static void
data_factory_constructed (GObject *object)
{
	EDataFactoryClass *class;
	EDataFactory *data_factory;
	GList *list, *link;

	data_factory = E_DATA_FACTORY (object);
	class = E_DATA_FACTORY_GET_CLASS (data_factory);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_data_factory_parent_class)->constructed (object);

	/* Collect all backend factories into a hash table. */

	list = e_extensible_list_extensions (
		E_EXTENSIBLE (object), class->backend_factory_type);

	for (link = list; link != NULL; link = g_list_next (link)) {
		EBackendFactory *backend_factory;
		const gchar *hash_key;

		backend_factory = E_BACKEND_FACTORY (link->data);
		hash_key = e_backend_factory_get_hash_key (backend_factory);

		if (hash_key != NULL) {
			g_hash_table_insert (
				data_factory->priv->backend_factories,
				g_strdup (hash_key),
				g_object_ref (backend_factory));
			g_debug (
				"Registering %s ('%s')\n",
				G_OBJECT_TYPE_NAME (backend_factory),
				hash_key);
		}
	}

	g_list_free (list);
}

static gboolean
data_factory_initable_init (GInitable *initable,
			    GCancellable *cancellable,
			    GError **error)
{
	EDataFactory *data_factory;

	data_factory = E_DATA_FACTORY (initable);

	data_factory->priv->registry = e_source_registry_new_sync (
		cancellable, error);

	return (data_factory->priv->registry != NULL);
}

static void
e_data_factory_initable_init (GInitableIface *iface)
{
	iface->init = data_factory_initable_init;
}

static void
e_data_factory_class_init (EDataFactoryClass *class)
{
	GObjectClass *object_class;
	EDBusServerClass *dbus_server_class;

	g_type_class_add_private (class, sizeof (EDataFactoryPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->get_property = e_data_factory_get_property;
	object_class->dispose = data_factory_dispose;
	object_class->finalize = data_factory_finalize;
	object_class->constructed = data_factory_constructed;

	class->backend_factory_type = E_TYPE_BACKEND_FACTORY;

	dbus_server_class = E_DBUS_SERVER_CLASS (class);
	dbus_server_class->bus_acquired = data_factory_bus_acquired;
	dbus_server_class->bus_name_lost = data_factory_bus_name_lost;
	dbus_server_class->quit_server = data_factory_quit_server;

	g_object_class_install_property (
		object_class,
		PROP_REGISTRY,
		g_param_spec_object (
			"registry",
			"Registry",
			"Data source registry",
			E_TYPE_SOURCE_REGISTRY,
			G_PARAM_READABLE |
			G_PARAM_STATIC_STRINGS));
}

static void
e_data_factory_init (EDataFactory *data_factory)
{
	data_factory->priv = E_DATA_FACTORY_GET_PRIVATE (data_factory);

	g_mutex_init (&data_factory->priv->mutex);
	g_mutex_init (&data_factory->priv->subprocess_watched_ids_lock);
	g_rec_mutex_init (&data_factory->priv->connections_lock);
	g_mutex_init (&data_factory->priv->watched_names_lock);
	g_mutex_init (&data_factory->priv->spawn_subprocess_lock);

	data_factory->priv->backend_factories = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_object_unref);

	data_factory->priv->subprocess_helpers = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) data_factory_subprocess_helper_free);

	data_factory->priv->subprocess_watched_ids = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) NULL);

	data_factory->priv->connections = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_ptr_array_unref);

	data_factory->priv->watched_names = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) watched_names_value_free);

	data_factory->priv->spawn_subprocess_state = DATA_FACTORY_SPAWN_SUBPROCESS_NONE;
}

/**
 * e_data_factory_ref_backend_factory:
 * @data_factory: an #EDataFactory
 * @backend_name: a backend name
 * @extension_name: an extension name
 *
 * Returns the #EBackendFactory for "@backend_name:@extension_name", or
 * %NULL if no such factory is registered.
 *
 * The returned #EBackendFactory is referenced for thread-safety.
 * Unreference the #EBackendFactory with g_object_unref() when finished
 * with it.
 *
 * Returns: the #EBackendFactory for @hash_key, or %NULL
 *
 * Since: 3.6
 **/
EBackendFactory *
e_data_factory_ref_backend_factory (EDataFactory *data_factory,
				    const gchar *backend_name,
				    const gchar *extension_name)
{
	GHashTable *backend_factories;
	EBackendFactory *backend_factory;
	gchar *hash_key;

	g_return_val_if_fail (E_IS_DATA_FACTORY (data_factory), NULL);
	g_return_val_if_fail (backend_name != NULL && *backend_name != '\0', NULL);
	g_return_val_if_fail (extension_name != NULL && *extension_name != '\0', NULL);

	/* It should be safe to lookup backend factories without a mutex
	 * because once initially populated the hash table remains fixed.
	 *
	 * XXX Which might imply the returned factory doesn't *really* need
	 *     to be referenced for thread-safety, but better to do it when
	 *     not really needed than wish we had in the future. */

	hash_key =  g_strdup_printf ("%s:%s", backend_name, extension_name);
	backend_factories = data_factory->priv->backend_factories;
	backend_factory = g_hash_table_lookup (backend_factories, hash_key);
	g_free (hash_key);

	if (backend_factory != NULL)
		g_object_ref (backend_factory);

	return backend_factory;
}

/**
 * e_data_factory_get_registry:
 * @data_factory: an #EDataFactory
 *
 * Returns the #ESourceRegistry owned by @data_factory.
 *
 * Returns: the #ESourceRegistry
 *
 * Since: 3.16
 **/
ESourceRegistry *
e_data_factory_get_registry (EDataFactory *data_factory)
{
	g_return_val_if_fail (E_IS_DATA_FACTORY (data_factory), NULL);

	return data_factory->priv->registry;
}

/**
 * e_data_factory_construct_path:
 * @data_factory: an #EDataFactory
 *
 * Returns a new and unique object path for a D-Bus interface based
 * in the data object path prefix of the @data_factory
 *
 * Returns: a newly allocated string, representing the object path for
 *          the D-Bus interface.
 *
 * Since: 3.16
 **/
gchar *
e_data_factory_construct_path (EDataFactory *data_factory)
{
	EDataFactoryClass *class;
	static volatile gint counter = 1;

	g_return_val_if_fail (E_IS_DATA_FACTORY (data_factory), NULL);

	g_atomic_int_inc (&counter);

	class = E_DATA_FACTORY_GET_CLASS (data_factory);
	g_return_val_if_fail (class->data_object_path_prefix != NULL, NULL);

	return g_strdup_printf (
		"%s/%d/%u",
		class->data_object_path_prefix, getpid (), counter);
}

static void
data_factory_spawn_subprocess_backend (EDataFactory *data_factory,
				       GDBusMethodInvocation *invocation,
				       const gchar *uid,
				       const gchar *extension_name,
				       const gchar *subprocess_path)
{
	DataFactorySubprocessHelper *helper;
	DataFactorySubprocessData *sd = NULL;
	EBackendFactory *backend_factory = NULL;
	EDataFactoryClass *class;
	ESource *source;
	EDataFactoryPrivate *priv;
	GError *error = NULL;
	GSubprocess *subprocess;
	guint watched_id = 0;
	gchar *backend_name = NULL;
	gchar *subprocess_helpers_hash_key;
	const gchar *factory_name;
	const gchar *filename;
	const gchar *type_name;

	g_return_if_fail (E_IS_DATA_FACTORY (data_factory));
	g_return_if_fail (invocation != NULL);
	g_return_if_fail (uid != NULL && *uid != '\0');
	g_return_if_fail (extension_name != NULL && *extension_name != '\0');
	g_return_if_fail (subprocess_path != NULL && *subprocess_path != '\0');

	priv = data_factory->priv;

	source = e_source_registry_ref_source (priv->registry, uid);
	if (source && e_source_has_extension (source, extension_name)) {
		ESourceBackend *extension;

		extension = e_source_get_extension (source, extension_name);
		backend_name = e_source_backend_dup_backend_name (extension);
	}
	g_clear_object (&source);

	if (backend_name && *backend_name) {
		backend_factory = e_data_factory_ref_backend_factory (
			data_factory, backend_name, extension_name);
	}

	g_free (backend_name);

	if (backend_factory) {
		type_name = G_OBJECT_TYPE_NAME (backend_factory);

		class = E_DATA_FACTORY_GET_CLASS (data_factory);
		factory_name = class->get_factory_name (backend_factory);

		subprocess_helpers_hash_key = data_factory_dup_subprocess_helper_hash_key (
			factory_name, extension_name, uid, e_backend_factory_share_subprocess (backend_factory));

		g_mutex_lock (&priv->mutex);
		helper = g_hash_table_lookup (
			priv->subprocess_helpers,
			subprocess_helpers_hash_key);
		g_mutex_unlock (&priv->mutex);

		filename = e_backend_factory_get_module_filename (backend_factory);

		if (helper != NULL) {
			data_factory_call_subprocess_backend_create_sync (
				data_factory,
				helper->proxy,
				invocation,
				uid,
				helper->bus_name,
				type_name,
				extension_name,
				filename);

			g_object_unref (backend_factory);
			g_free (subprocess_helpers_hash_key);

			return;
		}

		g_mutex_lock (&priv->spawn_subprocess_lock);
		if (priv->spawn_subprocess_state != DATA_FACTORY_SPAWN_SUBPROCESS_BLOCKED)
			priv->spawn_subprocess_state = DATA_FACTORY_SPAWN_SUBPROCESS_BLOCKED;
		g_mutex_unlock (&priv->spawn_subprocess_lock);

		sd = data_factory_subprocess_data_new (
			data_factory,
			invocation,
			uid,
			factory_name,
			type_name,
			extension_name,
			filename,
			subprocess_helpers_hash_key);

		g_object_unref (backend_factory);
		g_free (subprocess_helpers_hash_key);

		watched_id = g_bus_watch_name (
			G_BUS_TYPE_SESSION,
			sd->bus_name,
			G_BUS_NAME_WATCHER_FLAGS_NONE,
			data_factory_subprocess_appeared_cb,
			data_factory_subprocess_vanished_cb,
			sd,
			(GDestroyNotify) data_factory_subprocess_data_free);

		g_mutex_lock (&priv->subprocess_watched_ids_lock);
		g_hash_table_insert (priv->subprocess_watched_ids, g_strdup (sd->bus_name), GUINT_TO_POINTER (watched_id));
		g_mutex_unlock (&priv->subprocess_watched_ids_lock);

		subprocess = g_subprocess_new (
			G_SUBPROCESS_FLAGS_NONE,
			&error,
			subprocess_path,
			"--bus-name", sd->bus_name,
			"--own-path", sd->path,
			NULL);

		g_object_unref (subprocess);
	} else {
		error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Backend factory for source '%s' and extension '%s' cannot be found."),
			uid, extension_name);
	}

	if (error != NULL) {
		g_rec_mutex_lock (&priv->connections_lock);
		if (g_hash_table_size (priv->connections) == 0)
			e_dbus_server_release (E_DBUS_SERVER (data_factory));
		g_rec_mutex_unlock (&priv->connections_lock);

		if (sd) {
			g_mutex_lock (&priv->subprocess_watched_ids_lock);
			g_hash_table_remove (priv->subprocess_watched_ids, sd->bus_name);
			g_mutex_unlock (&priv->subprocess_watched_ids_lock);
		}

		if (watched_id)
			g_bus_unwatch_name (watched_id);

		g_dbus_method_invocation_take_error (invocation, error);

		g_mutex_lock (&priv->spawn_subprocess_lock);
		priv->spawn_subprocess_state = DATA_FACTORY_SPAWN_SUBPROCESS_NONE;
		g_cond_signal (&priv->spawn_subprocess_cond);
		g_mutex_unlock (&priv->spawn_subprocess_lock);
	}
}

static gpointer
data_factory_spawn_subprocess_backend_in_thread (gpointer user_data)
{
	DataFactorySpawnSubprocessBackendThreadData *data = user_data;
	EDataFactory *data_factory = data->data_factory;
	GDBusMethodInvocation *invocation = data->invocation;
	const gchar *uid = data->uid;
	const gchar *extension_name = data->extension_name;
	const gchar *subprocess_path = data->subprocess_path;

	g_mutex_lock (&data_factory->priv->spawn_subprocess_lock);
	while (data_factory->priv->spawn_subprocess_state == DATA_FACTORY_SPAWN_SUBPROCESS_BLOCKED) {
		g_cond_wait (
			&data_factory->priv->spawn_subprocess_cond,
			&data_factory->priv->spawn_subprocess_lock);
	}

	data_factory->priv->spawn_subprocess_state = DATA_FACTORY_SPAWN_SUBPROCESS_BLOCKED;
	g_mutex_unlock (&data_factory->priv->spawn_subprocess_lock);

	data_factory_spawn_subprocess_backend (
		data_factory, invocation, uid, extension_name, subprocess_path);

	data_factory_spawn_subprocess_backend_thread_data_free (data);

	return NULL;
}

/**
 * e_data_factory_spawn_subprocess_backend:
 * @data_factory: an #EDataFactory
 * @invocation: a #GDBusMethodInvcation
 * @uid: an #ESource UID
 * @extension_name: an extension name
 * @subprocess_path: a path of an executable responsible for running the subprocess
 *
 * Spawns a new subprocess for a backend type and returns the object path
 * of the new subprocess to the client, in the way the client can talk
 * directly to the running backend. If the backend already has a subprocess
 * running, the used object path is returned to the client.
 *
 * Since: 3.16
 **/
void
e_data_factory_spawn_subprocess_backend (EDataFactory *data_factory,
					 GDBusMethodInvocation *invocation,
					 const gchar *uid,
					 const gchar *extension_name,
					 const gchar *subprocess_path)
{
	GThread *thread;
	DataFactorySpawnSubprocessBackendThreadData *data;

	g_return_if_fail (E_IS_DATA_FACTORY (data_factory));

	/* Make sure the server will not quit due to inactivity while
	   the subprocess is opening */
	g_rec_mutex_lock (&data_factory->priv->connections_lock);
	if (g_hash_table_size (data_factory->priv->connections) == 0)
		e_dbus_server_hold (E_DBUS_SERVER (data_factory));
	g_rec_mutex_unlock (&data_factory->priv->connections_lock);

	data = data_factory_spawn_subprocess_backend_thread_data_new (
		data_factory, invocation, uid, extension_name, subprocess_path);

	thread = g_thread_new ("Spawn-Subprocess-Backend",
		data_factory_spawn_subprocess_backend_in_thread, data);

	g_thread_unref (thread);
}
