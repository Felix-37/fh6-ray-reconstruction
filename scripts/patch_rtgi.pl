#!/usr/bin/perl
# Patches the disassembly of Forza Horizon 6's RTGI_TemporalFilter (CRC 0x4DAF8A48) into one of
# the rr-forza variants. Every edit is anchored on an exact instruction that must appear exactly
# once; otherwise the script fails and the build stops.
#   hist2 / hist4 : history cap  N = umin(cb0[21].w + 1, previous count)  multiplied by 2 / 4
#   nofilter      : blend weight w = 0, so the output is the current frame (history count still
#                   written as before, which keeps switching back to the original seamless)
#   cap2 / cap4   : history cap  N = umin(cb0[21].w + 1, 2 / 4): a short pre-accumulation that
#                   lowers the raw noise handed to DLSS-RR while keeping its correlation short
#   nofilter_r16 / r8 / r4 : nofilter plus a lower radiance cap. The shader caps every GI sample at
#                   r + g + b <= 32 (saturate(sum / 32) * 32, then rgb rescaled: lines ~1175 and ~2185
#                   of the disassembly, no-history and main path). Fireflies are rare rays that hit a
#                   very bright emitter (a street lamp): an ABSOLUTE cap removes them where a relative
#                   spatial clamp cannot (with 1 sample per pixel at night, legit samples are sparse
#                   spikes too). Biased: it also clips legit bright indirect light above the cap.
#   nofilter_iso4 / iso2 / iso1 : nofilter plus a clamp inside the 3x3 resolve of the raw samples (shared
#                   memory sh_Radiance_hitT): the brightest contributing sample is scaled down to k x the
#                   second brightest (k = 4 / 2 / 1). A firefly is one lone sample; the resolve would spread it
#                   over 3x3 (then upscale), which is why clamps after the filter could not tell it apart.
#                   Measured useless before it: the absolute caps r16/r8/r4 (dim scenes never reach them).
#   <variant>_reset (and orig_reset) : the same variant, plus every pixel's previous history count
#                   read as 0 (%2243): the frame acts as if the whole screen had just come into view
#                   (both the blend and the count written for the spatial filter). Used for ONE frame by
#                   the "Medir desoclusion" bench, never as a setting.
# Usage: patch_rtgi.pl <variant> <input.ll> <output.ll>
use strict;
use warnings;

my ($variant, $in, $out) = @ARGV;
die "usage: patch_rtgi.pl <orig_reset|hist2|hist4|nofilter|cap2|cap4|nofilter_r16|nofilter_r8|nofilter_r4|nofilter_iso4|nofilter_iso2|nofilter_iso1>[_reset] <input.ll> <output.ll>\n" unless defined $out;
my $reset = $variant =~ s/_reset$//;

open my $fh, '<', $in or die "cannot read $in: $!\n";
binmode $fh;
local $/;
my $text = <$fh>;
close $fh;
# The build disassembles with `dxc -dumpbin -Fc` (LF line ends); the patches were written against a CRLF
# dump. Normalise to CRLF so both give byte-identical variants.
$text =~ s/\r?\n/\r\n/g;

sub replace_once {
    my ($ref, $from, $to) = @_;
    # The disassembly has CRLF line ends; the anchor must still match a whole line.
    my $count = () = $$ref =~ /^\Q$from\E(?=\r?$)/mg;
    die "anchor found $count times (expected 1): $from\n" unless $count == 1;
    $to =~ s/\n/\r\n/g;
    $$ref =~ s/^\Q$from\E(?=\r?$)/$to/m;
}

if ($variant eq 'orig') {
    die "orig only exists as orig_reset\n" unless $reset;
} elsif ($variant eq 'hist2' || $variant eq 'hist4') {
    my $k = $variant eq 'hist2' ? 2 : 4;
    replace_once(\$text, '  %2246 = add i32 %2245, 1',
                 "  %rr_cap_base = add i32 %2245, 1\n  %2246 = mul i32 %rr_cap_base, $k");
} elsif ($variant eq 'cap2' || $variant eq 'cap4') {
    my $k = $variant eq 'cap2' ? 2 : 4;
    # dx.op.binary.i32 opcode 40 = UMin, already declared by the shader (used right below for %2247).
    replace_once(\$text, '  %2246 = add i32 %2245, 1',
                 "  %rr_cap_base = add i32 %2245, 1\n  %2246 = call i32 \@dx.op.binary.i32(i32 40, i32 %rr_cap_base, i32 $k)");
} elsif ($variant eq 'nofilter') {
    replace_once(\$text, '  %2260 = fmul fast float %2259, %2006',
                 '  %2260 = fmul fast float %2259, 0.000000e+00');
} elsif ($variant =~ /^nofilter_iso([124])$/) {
    my $k = sprintf('%.6e', $1);
    replace_once(\$text, '  %2260 = fmul fast float %2259, %2006',
                 '  %2260 = fmul fast float %2259, 0.000000e+00');
    # The 9 raw samples of the 3x3 resolve (weight, rgb loaded from sh_Radiance_hitT), in code order.
    my @s = ([1110, 1116, 1122, 1128], [1203, 1209, 1215, 1221], [1302, 1308, 1314, 1320],
             [1400, 1406, 1412, 1418], [1446, 1452, 1458, 1464], [1544, 1550, 1556, 1562],
             [1643, 1649, 1655, 1661], [1741, 1747, 1753, 1759], [1839, 1845, 1851, 1857]);
    my $c = '';
    for my $i (0 .. 8) {
        my ($w, $r, $g, $b) = @{$s[$i]};
        $c .= "  %rr_l${i}a = fadd fast float %$r, %$g\n";
        $c .= "  %rr_l$i = fadd fast float %rr_l${i}a, %$b\n";
        $c .= "  %rr_p$i = fcmp fast ogt float %$w, 0.000000e+00\n";
        $c .= "  %rr_v$i = select i1 %rr_p$i, float %rr_l$i, float 0.000000e+00\n";
        $c .= "  %rr_n$i = select i1 %rr_p$i, float 1.000000e+00, float 0.000000e+00\n";
        $c .= "  %rr_c${i}r = fmul fast float %$w, %$r\n";
        $c .= "  %rr_c${i}g = fmul fast float %$w, %$g\n";
        $c .= "  %rr_c${i}b = fmul fast float %$w, %$b\n";
        if ($i == 0) {
            $c .= "  %rr_m1_0 = fadd fast float %rr_v0, 0.000000e+00\n";
            $c .= "  %rr_m2_0 = fmul fast float %rr_v0, 0.000000e+00\n";
            $c .= "  %rr_cnt_0 = fadd fast float %rr_n0, 0.000000e+00\n";
            $c .= "  %rr_xr_0 = fadd fast float %rr_c0r, 0.000000e+00\n";
            $c .= "  %rr_xg_0 = fadd fast float %rr_c0g, 0.000000e+00\n";
            $c .= "  %rr_xb_0 = fadd fast float %rr_c0b, 0.000000e+00\n";
            next;
        }
        my $p = $i - 1;
        $c .= "  %rr_gt$i = fcmp fast ogt float %rr_v$i, %rr_m1_$p\n";
        $c .= "  %rr_mx$i = call float \@dx.op.binary.f32(i32 35, float %rr_m2_$p, float %rr_v$i)\n";
        $c .= "  %rr_m2_$i = select i1 %rr_gt$i, float %rr_m1_$p, float %rr_mx$i\n";
        $c .= "  %rr_m1_$i = select i1 %rr_gt$i, float %rr_v$i, float %rr_m1_$p\n";
        $c .= "  %rr_cnt_$i = fadd fast float %rr_cnt_$p, %rr_n$i\n";
        for my $ch ('r', 'g', 'b') {
            $c .= "  %rr_x${ch}_$i = select i1 %rr_gt$i, float %rr_c$i$ch, float %rr_x${ch}_$p\n";
        }
    }
    # Only the brightest sample can exceed k x the second brightest (k >= 1): scale it down to that
    # limit, i.e. remove (1 - limit / max) of its weighted contribution. Needs 3+ contributing samples
    # (thin geometry with only the centre valid is left alone); the weight sum is unchanged.
    $c .= "  %rr_lim = fmul fast float %rr_m2_8, $k\n";
    $c .= "  %rr_big = fcmp fast ogt float %rr_m1_8, %rr_lim\n";
    $c .= "  %rr_many = fcmp fast oge float %rr_cnt_8, 3.000000e+00\n";
    $c .= "  %rr_on = and i1 %rr_big, %rr_many\n";
    $c .= "  %rr_den = call float \@dx.op.binary.f32(i32 35, float %rr_m1_8, float 0x3810000000000000)\n";
    $c .= "  %rr_keep = fdiv fast float %rr_lim, %rr_den\n";
    $c .= "  %rr_cut1 = fsub fast float 1.000000e+00, %rr_keep\n";
    $c .= "  %rr_cut = select i1 %rr_on, float %rr_cut1, float 0.000000e+00\n";
    $c .= "  %rr_dr = fmul fast float %rr_cut, %rr_xr_8\n";
    $c .= "  %rr_dg = fmul fast float %rr_cut, %rr_xg_8\n";
    $c .= "  %rr_db = fmul fast float %rr_cut, %rr_xb_8\n";
    replace_once(\$text, '  %1878 = fmul fast float %1874, %1839', $c . '  %1878 = fmul fast float %1874, %1839');
    # Final sums: %1885 = first channel (sample offset 0), %1883 = second, %1881 = third.
    replace_once(\$text, '  %1886 = fdiv fast float %1885, %1875', "  %rr_s0 = fsub fast float %1885, %rr_dr\n  %1886 = fdiv fast float %rr_s0, %1875");
    replace_once(\$text, '  %1887 = fdiv fast float %1883, %1875', "  %rr_s1 = fsub fast float %1883, %rr_dg\n  %1887 = fdiv fast float %rr_s1, %1875");
    replace_once(\$text, '  %1888 = fdiv fast float %1881, %1875', "  %rr_s2 = fsub fast float %1881, %rr_db\n  %1888 = fdiv fast float %rr_s2, %1875");
} elsif ($variant =~ /^nofilter_r(16|8|4)$/) {
    my %inv = (16 => '6.250000e-02', 8 => '1.250000e-01', 4 => '2.500000e-01');
    my %cap = (16 => '1.600000e+01', 8 => '8.000000e+00', 4 => '4.000000e+00');
    my $c = $1;
    replace_once(\$text, '  %2260 = fmul fast float %2259, %2006',
                 '  %2260 = fmul fast float %2259, 0.000000e+00');
    replace_once(\$text, '  %985 = fmul fast float %984, 3.125000e-02', "  %985 = fmul fast float %984, $inv{$c}");
    replace_once(\$text, '  %987 = fmul fast float %986, 3.200000e+01', "  %987 = fmul fast float %986, $cap{$c}");
    replace_once(\$text, '  %1896 = fmul fast float %1895, 3.125000e-02', "  %1896 = fmul fast float %1895, $inv{$c}");
    replace_once(\$text, '  %1898 = fmul fast float %1897, 3.200000e+01', "  %1898 = fmul fast float %1897, $cap{$c}");
} else {
    die "unknown variant $variant\n";
}

if ($reset) {
    replace_once(\$text, '  %2243 = extractvalue %dx.types.ResRet.i32 %2242, 0',
                 "  %rr_prev_count = extractvalue %dx.types.ResRet.i32 %2242, 0\n  %2243 = and i32 %rr_prev_count, 0");
}

open my $oh, '>', $out or die "cannot write $out: $!\n";
print $oh $text;
close $oh;
