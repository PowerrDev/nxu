#!/usr/bin/perl
#
# Run a command with a hard wall-clock limit: qemu_watchdog.pl <seconds> <command> [args...]
#
# `perl -e 'alarm N; exec @ARGV'` looks equivalent but is not: QEMU installs its
# own SIGALRM handler, so the alarm never stops it and a hung guest hangs the
# test run. This forks the command and SIGKILLs it from the parent instead.
#
# Exits with the command's own status, or 124 (as coreutils timeout does) if the
# limit was hit.

use strict;
use warnings;

my $seconds = shift @ARGV;
die "usage: qemu_watchdog.pl <seconds> <command> [args...]\n" unless defined $seconds && @ARGV;

my $pid = fork();
die "fork: $!\n" unless defined $pid;

if ($pid == 0) {
	exec @ARGV or die "exec $ARGV[0]: $!\n";
}

$SIG{ALRM} = sub {
	kill 9, $pid;
	waitpid($pid, 0);
	exit 124;
};

alarm $seconds;
waitpid($pid, 0);
alarm 0;

if ($? & 127) {
	exit 128 + ($? & 127);
}

exit($? >> 8);
