package My::Suite::Connect;

@ISA = qw(My::Suite);

return "No CONNECT engine" unless $ENV{HA_CONNECT_SO} or
                                  $::mysqld_variables{'connect'} eq "ON";

# RECOMPILE_FOR_EMBEDDED also means that a plugin
# cannot be dynamically loaded into embedded
return "Not run for embedded server" if $::opt_embedded_server and
                                        $ENV{HA_CONNECT_SO};

sub is_default { 1 }

# To allow the lsan suppression on unixodbc to work
# the llvm-symbolizer needs to be are of the address
# resolution even after the HA_CONNECT_SO has been dlclosed.

# Check OS and file existence
if ($^O =~ /linux|darwin|unix/i && -x "/usr/bin/readelf")
{
  my $asan_symbols = 0;
  my $file = $::plugindir . '/' .  $ENV{HA_CONNECT_SO};

  # Open readelf -s output and scan for __asan symbols
  open(my $sh, '-|', "readelf -s '$file'")
      or die "Failed to run readelf: $!\n";
  while (<$sh>) { $asan_symbols = 1 if /__asan/; }
  close($sh);

  if ($asan_symbols && -x "/usr/bin/ldd")
  {
    # To allow the lsan suppression on unixodbc to work
    # the llvm-symbolizer needs to be aware of the address
    # resolution even after the HA_CONNECT_SO has been dlclosed.

    my $lib_path;
    open(my $ldd, '-|', "ldd $file") or die "Failed to run ldd: $!";

    while (<$ldd>)
    {
        chomp;
        # Example ldd line: libodbc.so.2 => /usr/lib/x86_64-linux-gnu/libodbc.so.2 (0x00007f...)
        if (/libodbc\.so(?:\.\d+)*\s+=>\s+(\S+)/)
	{
            $lib_path = $1;
            last;  # stop after the first match
        }
    }
    close $ldd;

    # assuming odbcinst is in the same path so we can check
    # if it has fixed version.
    my $libodbcinst = $lib_path;
    $libodbcinst =~ s/odbc/odbcinst/;
    my $leakfixed= 0;
    if ( -f $libodbcinst )
    {
      open(my $sh, '-|', "readelf -s '$libodbcinst'")
          or die "Failed to run readelf: $!\n";
      while (<$sh>) { $leakfixed = 1 if /inst_logClose/; }
      close($sh);
    }
    if (!$leakfixed && $lib_path)
    {
      $ENV{LD_PRELOAD} = $ENV{LD_PRELOAD} ? "$ENV{LD_PRELOAD}:$lib_path" : $lib_path;
    }
  }
}

bless { };

