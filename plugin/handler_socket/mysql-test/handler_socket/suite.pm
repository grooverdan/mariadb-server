package My::Suite::Handler_socket_test;

@ISA = qw(My::Suite);

return "No Handler_socket plugin" unless $ENV{HANDLERSOCKET_SO};

return "Not run for embedded server" if $::opt_embedded_server;

use Socket;

my $port = 9001;
my $paddr = sockaddr_in($port, INADDR_ANY);
my $protocol = getprotobyname("tcp");
socket(SOCK, PF_INET, SOCK_STREAM, $protocol);

if(!connect(SOCK, $paddr))
{
  $ENV{'HANDLER_PORT'} = $port;
  $ENV{'HANDLER_PORT_WR'} = $port + 1;
}
else
{
  return "Port $port not available for test of handlersocket";
}

push @::global_suppressions,
  (
    qr(open_files_limit is too small\.)
  );

sub is_default { 1 }

bless { };

