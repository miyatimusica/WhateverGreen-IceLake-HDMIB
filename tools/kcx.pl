#!/usr/bin/perl
# kcx.pl - extrae entradas (kexts) de un Kernel Collection (Mach-O fileset) de macOS.
# Uso:   perl kcx.pl <archivo.kc> <carpeta_salida> <texto_a_buscar> [otro_texto ...]
# Ej.:   perl kcx.pl /System/Library/KernelCollections/BootKernelExtensions.kc ~/Desktop/kcx AppleIntelICL
# Sin <texto_a_buscar> solo lista las entradas que contiene el KC.
use strict;
use warnings;

my ($kc, $outdir, @pats) = @ARGV;
die "uso: perl kcx.pl <archivo.kc> <carpeta_salida> <texto> [texto ...]\n" unless defined $kc;

open(my $fh, '<:raw', $kc) or die "no puedo abrir $kc: $!\n";
my $fsize = -s $kc;

sub rd {
    my ($off, $len) = @_;
    return '' if $off < 0 || $len <= 0 || $off + $len > $fsize;
    seek($fh, $off, 0) or return '';
    my $buf = '';
    my $n = read($fh, $buf, $len);
    return defined $n ? $buf : '';
}
sub u32 { return unpack('V', substr($_[0], $_[1], 4)); }
sub u64 { return unpack('Q<', substr($_[0], $_[1], 8)); }
sub cstr { my $s = $_[0]; $s =~ s/\0.*//s; return $s; }

my $hdr = rd(0, 32);
die "archivo demasiado corto\n" if length($hdr) < 32;
my $magic = u32($hdr, 0);
die sprintf("magic 0x%08x: no es un Mach-O de 64 bits sin comprimir (puede estar comprimido o ser fat).\n"
          . "Ejecuta:  file '%s'  y envíame la salida.\n", $magic, $kc) unless $magic == 0xfeedfacf;
my $ftype = u32($hdr, 12);
my $ncmds = u32($hdr, 16);
my $szcmd = u32($hdr, 20);
printf "Mach-O 64, filetype=0x%x (0xc = fileset), ncmds=%d, tamano=%d bytes\n", $ftype, $ncmds, $fsize;

my $lcs = rd(32, $szcmd);
my (@top_segs, @entries);
my $p = 0;
for (1 .. $ncmds) {
    last if $p + 8 > length($lcs);
    my ($cmd, $cs) = (u32($lcs, $p), u32($lcs, $p + 4));
    last if $cs < 8;
    if ($cmd == 0x19) {         # LC_SEGMENT_64 de nivel superior
        push @top_segs, {
            name => cstr(substr($lcs, $p + 8, 16)),
            vm   => u64($lcs, $p + 24), vmsz => u64($lcs, $p + 32),
            fo   => u64($lcs, $p + 40), fsz  => u64($lcs, $p + 48),
        };
    } elsif ($cmd == 0x80000035) {   # LC_FILESET_ENTRY
        my $off = u32($lcs, $p + 24);
        push @entries, {
            vm => u64($lcs, $p + 8), fo => u64($lcs, $p + 16),
            id => cstr(substr($lcs, $p + $off, $cs - $off)),
        };
    }
    $p += $cs;
}
printf "segmentos de nivel superior: %d, entradas (kexts): %d\n", scalar(@top_segs), scalar(@entries);

# vmaddr -> offset de archivo usando los segmentos de nivel superior
sub vm2fo {
    my ($vm) = @_;
    for my $s (@top_segs) {
        next unless $s->{fsz} > 0;
        return ($s->{fo} + ($vm - $s->{vm}), $s->{fo} + $s->{fsz})
            if $vm >= $s->{vm} && $vm < $s->{vm} + $s->{fsz};
    }
    return (undef, undef);
}

unless (@pats) {
    print "  $_->{id}\n" for @entries;
    exit 0;
}

mkdir $outdir unless -d $outdir;
my $found = 0;
for my $e (@entries) {
    next unless grep { index(lc $e->{id}, lc $_) >= 0 } @pats;
    $found++;
    my $dir = "$outdir/$e->{id}";
    mkdir $dir;
    open(my $ix, '>', "$dir/index.txt") or die "no puedo escribir en $dir: $!\n";
    printf $ix "entry %s\nheader vmaddr=0x%x fileoff=0x%x\n", $e->{id}, $e->{vm}, $e->{fo};

    my $eh = rd($e->{fo}, 32);
    if (length($eh) < 32 || u32($eh, 0) != 0xfeedfacf) {
        my ($fo2) = vm2fo($e->{vm});
        $eh = defined $fo2 ? rd($fo2, 32) : '';
    }
    if (length($eh) < 32 || u32($eh, 0) != 0xfeedfacf) {
        print $ix "ERROR: no encuentro la cabecera Mach-O de la entrada\n";
        close $ix;
        print "  [!] $e->{id}: cabecera no encontrada\n";
        next;
    }
    my ($enc, $esz) = (u32($eh, 16), u32($eh, 20));
    my $hoff = (u32(rd($e->{fo}, 4), 0) == 0xfeedfacf) ? $e->{fo} : (vm2fo($e->{vm}))[0];
    my $elcs = rd($hoff + 32, $esz);
    my $q = 0;
    for (1 .. $enc) {
        last if $q + 8 > length($elcs);
        my ($cmd, $cs) = (u32($elcs, $q), u32($elcs, $q + 4));
        last if $cs < 8;
        if ($cmd == 0x19) {
            my $sn   = cstr(substr($elcs, $q + 8, 16));
            my $svm  = u64($elcs, $q + 24);
            my $svsz = u64($elcs, $q + 32);
            my $nsec = u32($elcs, $q + 64);
            my ($fo, $lim) = vm2fo($svm);
            printf $ix "\nSEGMENT %s vmaddr=0x%x vmsize=0x%x\n", $sn, $svm, $svsz;
            for my $i (0 .. $nsec - 1) {
                my $b = $q + 72 + $i * 80;
                last if $b + 80 > length($elcs);
                printf $ix "  section %s,%s addr=0x%x size=0x%x\n",
                    cstr(substr($elcs, $b + 16, 16)), cstr(substr($elcs, $b, 16)),
                    u64($elcs, $b + 32), u64($elcs, $b + 40);
            }
            if (defined $fo && $svsz > 0) {
                my $len = $svsz;
                $len = $lim - $fo if $fo + $len > $lim;
                my $data = rd($fo, $len);
                (my $fn = $sn) =~ s/[^A-Za-z0-9_]/_/g;
                open(my $o, '>:raw', "$dir/$fn.bin") or die;
                print $o $data;
                close $o;
                printf $ix "  -> %s.bin (%d bytes, fileoff=0x%x)\n", $fn, length($data), $fo;
            } else {
                print $ix "  (sin datos en archivo)\n";
            }
        } elsif ($cmd == 0x2) {      # LC_SYMTAB
            my ($symoff, $nsyms, $stroff, $strsz) = map { u32($elcs, $q + 8 + 4 * $_) } 0 .. 3;
            printf $ix "\nSYMTAB symoff=0x%x nsyms=%d stroff=0x%x strsize=%d\n", $symoff, $nsyms, $stroff, $strsz;
            my $sy = rd($symoff, $nsyms * 16);
            my $st = rd($stroff, $strsz);
            if (length($sy) == $nsyms * 16 && length($st) == $strsz && $nsyms > 0) {
                open(my $o1, '>:raw', "$dir/symtab.bin") or die; print $o1 $sy; close $o1;
                open(my $o2, '>:raw', "$dir/strtab.bin") or die; print $o2 $st; close $o2;
                print $ix "  -> symtab.bin / strtab.bin\n";
            } else {
                print $ix "  (offsets de simbolos fuera de rango; omitido)\n";
            }
        }
        $q += $cs;
    }
    close $ix;
    print "  [ok] $e->{id}\n";
}
print "\nentradas extraidas: $found\n";
print "Este KC no contiene ese texto; prueba con el otro KC.\n" if $found == 0;
