#!/usr/bin/perl

# Generate 'documents' in different encodings, from po files

if ($#ARGV < 0) {
    print "Usage: genline.pl pofile pofile ...\n";
    exit 1;
}

sub read_msgstr()
{
    my $str = "";
    while (<IN>) {
	if (m/^msgstr \"(.*)\"/) {
	    $str = $1;
	    if ($str eq "") {
		while (<IN>) {
		    if (m/\"(.*)\"/) {
			$str .= $1;
		    } else {
			last;
		    }
		}
	    }
	    return $str;
	}
    }
    return "";
}

$unknown = "x-unknown-1";
open OUT, ">test-lines.h";

print OUT <<END;
struct _l {
    char *type;
    char *line;
} test_lines[] = {
END

foreach $name (@ARGV) {
    if ($name =~ m@([^/]*).po$@) {
	$poname = $1;
	    
	open IN,"<$name";

	$header = read_msgstr;
	if ($header =~ /Content-Type:.*charset=([-a-zA-Z0-9]*)/i) {
	    $charset = $1;
	} else {
	    $charset = $unknown++;
	}

	while (!eof(IN)) {
	    $msg = read_msgstr;
	    if (length($msg) > 60 && length($msg) < 160) {
		print OUT "\t{ \"$charset\", \"$msg\" },\n";
		last;
	    }
#	    $msg =~ s/\\n/\n/gso;
#	    $msg =~ s/\\t/\t/gso;
#	    $msg =~ s/\\(.)/$1/gso;
#	    print OUT $msg." ";
	}
	close IN;
    } else {
	printf("ignoring $name, probably not intended\n");
    }
}

print OUT "};\n";
close OUT;
