#!/usr/bin/perl

use warnings;
use strict;

my $DIAMOND_SRC = "/home/nl35/research/diamond-src";
my $PROJECT_DIR = "$DIAMOND_SRC/apps/chat/DesktopChat";
my $JAVA_BINDINGS_DIR = "$DIAMOND_SRC/backend/src/bindings/java";

my $classpath = "$PROJECT_DIR/bin:$JAVA_BINDINGS_DIR/libs/javacpp.jar:$JAVA_BINDINGS_DIR/target/diamond-1.0-SNAPSHOT.jar";
my $nativePath = "$JAVA_BINDINGS_DIR/target/classes/x86-lib:$DIAMOND_SRC/backend/build";

$ENV{"LD_LIBRARY_PATH"} = $nativePath;
system("java -cp $classpath -Djava.library.path=$nativePath Main 0 client1 > client1.log 2> error1.log &");
system("java -cp $classpath -Djava.library.path=$nativePath Main 1.0 client2 > client2.log 2> error2.log &");
sleep(1);
system("./latency-measurements.pl");

#my $action = $ARGV[0];
#my $clientName = $ARGV[1];
#system("java -cp $classpath -Djava.library.path=$nativePath Main $action $clientName");
