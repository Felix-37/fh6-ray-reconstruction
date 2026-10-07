#!/usr/bin/perl
# Patches the disassembly of Forza Horizon 6's RTGI_SpatialFilter_Disk (CRC 0x209AB6A4) into one of
# the rr-forza variants. Every edit is anchored on an exact instruction that must appear exactly
# once; otherwise the script fails and the build stops.
#   assist4 / assist8 : "desoclusion asistida" for "RR completo". The game's filter reads the GI
#                   history count of each pixel (t28, %179) and widens its disk for young pixels.
#                   This variant keeps the game's filter for pixels that just came into view and fades
#                   it out as their history grows: out = lerp(filtered, centre, saturate((count - 1) / N)),
#                   N = 4 / 8 frames. Converged pixels get the centre sample (the "pass" variant), so
#                   DLSS-RR keeps doing all the work there.
# Usage: patch_rtgi_spatial.pl <assist4|assist8> <input.ll> <output.ll>
use strict;
use warnings;

my ($variant, $in, $out) = @ARGV;
die "usage: patch_rtgi_spatial.pl <assist4|assist8> <input.ll> <output.ll>\n" unless defined $out;

open my $fh, '<', $in or die "cannot read $in: $!\n";
binmode $fh;
local $/;
my $text = <$fh>;
close $fh;
# Same line-end normalisation as patch_rtgi.pl (the build disassembles with LF line ends).
$text =~ s/\r?\n/\r\n/g;

sub replace_once {
    my ($ref, $from, $to) = @_;
    my $count = () = $$ref =~ /^\Q$from\E(?=\r?$)/mg;
    die "anchor found $count times (expected 1): $from\n" unless $count == 1;
    $to =~ s/\n/\r\n/g;
    $$ref =~ s/^\Q$from\E(?=\r?$)/$to/m;
}

$variant =~ /^assist(4|8)$/ or die "unknown variant $variant\n";
my $inv = sprintf('%.6e', 1.0 / $1);

# Filtered outputs %194..%197 (centre %63..%66) and %198..%201 (centre %69..%72); count %179 is
# loaded in block %122, which dominates the store block %192.
my $c = '';
$c .= "  %rr_age1 = add nsw i32 %179, -1\n";
$c .= "  %rr_age = call i32 \@dx.op.binary.i32(i32 37, i32 %rr_age1, i32 0)\n";   # IMax(age, 0)
$c .= "  %rr_agef = sitofp i32 %rr_age to float\n";
$c .= "  %rr_t0 = fmul fast float %rr_agef, $inv\n";
$c .= "  %rr_t = call float \@dx.op.unary.f32(i32 7, float %rr_t0)\n";            # Saturate
my @pairs = ([194, 63], [195, 64], [196, 65], [197, 66], [198, 69], [199, 70], [200, 71], [201, 72]);
for my $p (@pairs) {
    my ($f, $ctr) = @$p;
    $c .= "  %rr_d$f = fsub fast float %$ctr, %rr_f$f\n";
    $c .= "  %$f = call float \@dx.op.tertiary.f32(i32 46, float %rr_t, float %rr_d$f, float %rr_f$f)\n";   # FMad
}
for my $p (@pairs) {
    my ($f) = @$p;
    my $acc = $f - 194 + 431;
    replace_once(\$text, "  %$f = fmul fast float %193, %$acc", "  %rr_f$f = fmul fast float %193, %$acc");
}
replace_once(\$text, '  %202 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4098, i32 1033 })  ; AnnotateHandle(res,props)  resource: RWTexture2D<4xF32>',
             $c . '  %202 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4098, i32 1033 })  ; AnnotateHandle(res,props)  resource: RWTexture2D<4xF32>');

# Declarations the original may not have.
$text .= "\r\ndeclare float \@dx.op.tertiary.f32(i32, float, float, float) #0\r\n" unless $text =~ /declare float \@dx\.op\.tertiary\.f32/;
$text .= "\r\ndeclare i32 \@dx.op.binary.i32(i32, i32, i32) #0\r\n" unless $text =~ /declare i32 \@dx\.op\.binary\.i32/;
$text .= "\r\ndeclare float \@dx.op.unary.f32(i32, float) #0\r\n" unless $text =~ /declare float \@dx\.op\.unary\.f32/;

open my $oh, '>', $out or die "cannot write $out: $!\n";
print $oh $text;
close $oh;
