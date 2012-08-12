#!/usr/bin/perl
use warnings FATAL=>"all";
use strict;
use Data::Dumper;

my %commiters= ();

my %commit_description = ();
(my $origin,my $target) = @ARGV;
print "generating between $origin and $target\n";
foreach my $commit (qx/git log --no-merges $origin..$target --format=format:%H/) {
	chomp $commit;
	my $analyzed_commit = {
		hash=>qx/git log --format=format:%H $commit^1..$commit/,
		author=>qx/git log --format=format:%aN $commit^1..$commit/,
		commiter=>qx/git log --format=format:%cN $commit^1..$commit/,
		subject=>qx/git log --format=format:%s $commit^1..$commit/,
		date=>qx/git log --format=format:%cD $commit^1..$commit/,
		unix_date=>qx/git log --format=format:%ct $commit^1..$commit/,
		stat=>join("",qx/git diff --stat $commit^1..$commit/),
		files=>[]
	};
	if($analyzed_commit->{author} eq "Olivier") {$analyzed_commit->{author} = "Olivier Tribout"}
	my @lines=split("\n",$analyzed_commit->{stat});
	pop @lines;
	foreach my $line (@lines) {
		$line=~/ ([^ ]*) | .*/;
		push @{$analyzed_commit->{files}},$1;

	}
	$commiters{$analyzed_commit->{commiter}}=1; 
	$commit_description{$analyzed_commit->{author}} = [] if not defined($commit_description{$analyzed_commit->{author}});
	push @{$commit_description{$analyzed_commit->{author}}},$analyzed_commit;
}
print "Commiters : ".join("; ",keys(%commiters))."\n";

my %filtered_commits=();
foreach my $author (keys %commit_description) {
	my @commit_list=();
	commit:foreach my $commit (@{$commit_description{$author}}) {
		foreach my $file (@{$commit->{files}}) {
			if($file!~/po$/ and $file!~/POTFILES.in$/ and $file!~/LINGUAS$/) {
				push @commit_list,$commit;
				next commit;
			}
		}
	}
	next if not @commit_list;
	$filtered_commits{$author}=[@commit_list];
}

foreach my $author (reverse sort({scalar(@{$filtered_commits{$a}}) <=> scalar(@{$filtered_commits{$b}})} keys(%filtered_commits))) {
	next if exists($commiters{$author});
	my @commits=reverse sort({$a->{unix_date}<=>$b->{unix_date}} @{$filtered_commits{$author}});
	print "===== $author : ".scalar(@commits)." commits. First commit: ".$commits[$#commits]->{date}." last commit: ".$commits[0]->{date}." =====\n\n";
	foreach my $commit (@commits) {
		print $commit->{hash}."\n";
		print $commit->{subject}."\n";
		print "Date : ".$commit->{date}."\n";
		print "Commited by : ".$commit->{commiter}."\n";
		print $commit->{stat}."\n";
		print "\n";
	}
}
