#!/usr/bin/perl
use strict;
use warnings;

my $file = $ARGV[0] or die "Usage: $0 <file>\n";

open my $fh, '<', $file or die "Cannot open $file: $!";
my $content = do { local $/; <$fh> };
close $fh;

# Zero-arg methods: replace fn.method() with ir_query::method(fn)
my @zero_arg = qw(
    hasLoopWithDecrement
    switchCaseCount
    hasSelfCall
    selfCallCount
    hasSimdInstruction
    hasMutualRecursion
    loadWidthsPresent
    hasBackwardLoop
    hasThreeWayBranch
    hasAllocation
    hasChildIndexArithmetic
    hasConvergingIndices
    hasSwapPattern
);

for my $m (@zero_arg) {
    $content =~ s/\bfn\.$m\(\)/ir_query::${m}(fn)/g;
}

# Methods with args: replace fn.method( with ir_query::method(fn, 
my @with_args = qw(
    hasAndMask
    hasBitwiseOr
    hasRightShift
    hasLeftShift
    hasSwitchOnByte
    callsFunction
    hasStructOffset
    hasStringLiteral
    hasPointerArithWithConst
    hasConstant
);

for my $m (@with_args) {
    $content =~ s/\bfn\.$m\(/ir_query::${m}(fn, /g;
}

open my $out, '>', $file or die "Cannot write $file: $!";
print $out $content;
close $out;

print "Done.\n";
