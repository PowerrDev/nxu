#!/usr/bin/perl
#
# Type into a guest's serial console as a script says, and pass everything
# the guest prints through to stdout:
#
#   serial_expect.pl <seconds> <script> <command> [args...]
#
# The command is QEMU with `-serial stdio`: its standard input is the console
# UART's receive side, its standard output the transmit side (standard error
# is merged into it). Used by the serial terminal test (kern/tests/serial_test.c)
# from tools/test_i386_boot.sh and tools/check.sh.
#
# Script lines, run in order; blank lines and lines starting with # are skipped:
#
#   wait <text>     wait until the guest has printed <text> (after whatever the
#                   last wait matched)
#   send <text>     type <text>
#   flood <count>   type <count> bytes: a, b, ... z, a, ...
#   done            print "serial_expect.pl: script complete", stop the guest
#                   and exit 0: for a guest that never exits by itself (a
#                   normal boot); the rest of its output is lost
#
# <text> takes the escapes \r \n \t \\ and \xHH, so control characters can be
# typed and their echo waited for.
#
# Exits with the command's own status once the script is done and the command
# has exited (0 at a done step); with 124 (as qemu_watchdog.pl and coreutils timeout do) if the
# limit was hit before that. A wait the guest never satisfies is reported on
# stderr before the 124.

use strict;
use warnings;

use IO::Select;
use POSIX ();

my $seconds = shift @ARGV;
my $script_path = shift @ARGV;
die "usage: serial_expect.pl <seconds> <script> <command> [args...]\n"
	unless defined $seconds && defined $script_path && @ARGV;

sub unescape {
	my ($text) = @_;
	$text =~ s/\\(x([0-9A-Fa-f]{2})|r|n|t|\\)/
		defined $2 ? chr(hex $2) : $1 eq 'r' ? "\r" : $1 eq 'n' ? "\n" : $1 eq 't' ? "\t" : "\\"/ge;
	return $text;
}

my @steps;
open(my $script, '<', $script_path) or die "serial_expect.pl: $script_path: $!\n";
while (my $line = <$script>) {
	chomp $line;
	next if $line =~ /^\s*(#|$)/;
	my ($verb, $argument) = $line =~ /^(\w+)(?:\s(.*))?$/ or die "serial_expect.pl: bad line: $line\n";
	if ($verb eq 'wait' || $verb eq 'send') {
		push @steps, [$verb, unescape($argument)];
	} elsif ($verb eq 'done') {
		push @steps, ['done', ''];
	} elsif ($verb eq 'flood') {
		push @steps, ['send', join('', map { chr(ord('a') + $_ % 26) } 0 .. $argument - 1)];
	} else {
		die "serial_expect.pl: unknown step: $verb\n";
	}
}
close $script;

$SIG{PIPE} = 'IGNORE';

pipe(my $guest_in_read, my $guest_in) or die "pipe: $!\n";
pipe(my $guest_out, my $guest_out_write) or die "pipe: $!\n";

my $pid = fork();
die "fork: $!\n" unless defined $pid;

if ($pid == 0) {
	close $guest_in;
	close $guest_out;
	open(STDIN, '<&', $guest_in_read) or die "stdin: $!\n";
	open(STDOUT, '>&', $guest_out_write) or die "stdout: $!\n";
	open(STDERR, '>&', $guest_out_write) or die "stderr: $!\n";
	exec @ARGV or die "exec $ARGV[0]: $!\n";
}

# Stopped from outside (tools/check.sh kills it once the pass lines show): the guest goes too.
$SIG{TERM} = sub { finish(143) };
$SIG{INT} = sub { finish(130) };

close $guest_in_read;
close $guest_out_write;
binmode $guest_out;
binmode STDOUT;
$| = 1;

my $deadline = time() + $seconds;
my $select = IO::Select->new($guest_out);
my $seen = '';
my $position = 0;
my $step = 0;

sub finish {
	my ($status) = @_;
	kill 9, $pid;
	waitpid($pid, 0);
	exit $status;
}

for (;;) {
	# Everything the script can do without waiting for the guest.
	while ($step < @steps) {
		my ($verb, $text) = @{$steps[$step]};
		if ($verb eq 'done') {
			print "\nserial_expect.pl: script complete\n";
			finish(0);
		} elsif ($verb eq 'send') {
			my $offset = 0;
			while ($offset < length $text) {
				my $written = syswrite($guest_in, $text, length($text) - $offset, $offset);
				last unless defined $written;
				$offset += $written;
			}
		} else {
			my $found = index($seen, $text, $position);
			last if $found < 0;
			$position = $found + length $text;
		}
		$step++;
	}

	my $left = $deadline - time();
	if ($left <= 0) {
		if ($step < @steps) {
			my $wanted = $steps[$step][1];
			$wanted =~ s/([^\x20-\x7e])/sprintf('\\x%02x', ord $1)/ge;
			print STDERR "serial_expect.pl: timed out waiting for \"$wanted\"\n";
		}
		finish(124);
	}

	next unless $select->can_read($left < 1 ? $left : 1);

	my $chunk;
	my $count = sysread($guest_out, $chunk, 65536);

	if (!$count) {
		# The guest is gone. A script that did not finish is a failure even if it exited cleanly.
		waitpid($pid, 0);
		my $status = $? & 127 ? 128 + ($? & 127) : $? >> 8;
		if ($step < @steps) {
			print STDERR "serial_expect.pl: the guest exited before the script finished\n";
			exit 125;
		}
		exit $status;
	}

	print $chunk;
	$seen .= $chunk;
}
