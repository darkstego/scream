#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <net/if.h>

#include "scream.h"
#include "network.h"
#include "shmem.h"

#include "raw.h"

#if PULSEAUDIO_ENABLE
#include "pulseaudio.h"
#endif

#if ALSA_ENABLE
#include "alsa.h"
#endif

static void show_usage(const char *arg0)
{
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage: %s [-u] [-p <port>] [-i <iface>] [-g <group>]\n", arg0);
  fprintf(stderr, "\n");
  fprintf(stderr, "         All command line options are optional. Default is to use\n");
  fprintf(stderr, "         multicast with group address 239.255.77.77, port 4010.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "         -u                        : Use unicast instead of multicast.\n");
  fprintf(stderr, "         -p <port>                 : Use <port> instead of default port 4010.\n");
  fprintf(stderr, "                                     Applies to both multicast and unicast.\n");
  fprintf(stderr, "         -i <iface>                : Use local interface <iface>. Either the IP\n");
  fprintf(stderr, "                                     or the interface name can be specified. In\n");
  fprintf(stderr, "                                     multicast mode, uses this interface for IGMP.\n");
  fprintf(stderr, "                                     In unicast, binds to this interface only.\n");
  fprintf(stderr, "         -g <group>                : Multicast group address. Multicast mode only.\n");
  fprintf(stderr, "         -m <ivshmem device path>  : Use shared memory device.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "         -o pulse|alsa|raw         : Send audio to PulseAudio, ALSA, or stdout.\n");
  fprintf(stderr, "         -d <device>               : ALSA device name. 'default' if not specified.\n");
  fprintf(stderr, "         -n <stream name>          : Pulseaudio stream name/description.\n");
  fprintf(stderr, "         -t <latency>              : Target latency in milliseconds. Defaults to 50ms.\n");
  fprintf(stderr, "                                     Only relevant for PulseAudio and ALSA output.\n");
  fprintf(stderr, "         -l <latency>              : Max latency in milliseconds. Defaults to 200ms.\n");
  fprintf(stderr, "                                     Only relevant for PulseAudio output.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "         -v                        : Be verbose.\n");
  fprintf(stderr, "\n");
  exit(1);
}


static in_addr_t get_interface(const char *name)
{
  int sockfd = socket(AF_INET,SOCK_DGRAM,0);
  struct ifreq ifr;
  in_addr_t addr = inet_addr(name);
  struct if_nameindex *ni;
  int i;

  if (addr != INADDR_NONE) {
    return addr;
  }

  if (strlen(name) >= sizeof(ifr.ifr_name)) {
    fprintf(stderr, "Too long interface name: %s\n\n", name);
    goto error_exit;
  }
  strcpy(ifr.ifr_name, name);

  sockfd = socket(AF_INET,SOCK_DGRAM,0);
  if (ioctl(sockfd, SIOCGIFADDR, &ifr) != 0) {
    fprintf(stderr, "Invalid interface: %s\n\n", name);
    goto error_exit;
  }
  close(sockfd);
  return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;

error_exit:
  ni = if_nameindex();
  fprintf(stderr, "Available interfaces:\n");
  for (i = 0; ni[i].if_name != NULL; i++) {
    strcpy(ifr.ifr_name, ni[i].if_name);
    if (ioctl(sockfd, SIOCGIFADDR, &ifr) == 0) {
      fprintf(stderr, "  %-10s (%s)\n", ni[i].if_name, inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
    }
  }
  exit(1);
}


int main(int argc, char*argv[]) {
  int error;

  // function pointer definition for receiver
  void (*receiver_rcv_fn)(receiver_data_t* receiver_data);
  receiver_data_t receiver_data;

  int (*output_send_fn)(receiver_data_t* receiver_data);

  // Command line options
  enum receiver_type receiver_mode = Multicast;

#if PULSEAUDIO_ENABLE
  enum output_type output_mode = Pulseaudio;
#elif ALSA_ENABLE
  enum output_type output_mode = Alsa;
#else
  enum output_type output_mode = Raw;
#endif

  char *multicast_group = NULL;
  char *ivshmem_device  = NULL;
  char *output          = NULL;
  char *alsa_device     = "default";
  char *stream_name     = "Audio";
  int target_latency_ms = 50;
  int max_latency_ms = 200;
  in_addr_t interface   = INADDR_ANY;
  uint16_t port         = DEFAULT_PORT;

  int opt;
  while ((opt = getopt(argc, argv, "i:g:p:m:o:d:n:t:l:uvh")) != -1) {
    switch (opt) {
    case 'i':
      interface = get_interface(optarg);
      break;
    case 'p':
      port = atoi(optarg);
      if (!port) show_usage(argv[0]);
      break;
    case 'u':
      receiver_mode = Unicast;
      break;
    case 'g':
      multicast_group = strdup(optarg);
      break;
    case 'm':
      receiver_mode = SharedMem;
      ivshmem_device = strdup(optarg);
      break;
    case 'o':
      output = strdup(optarg);
      if (strcmp(output,"pulse") == 0) output_mode = Pulseaudio;
      else if (strcmp(output,"alsa") == 0) output_mode = Alsa;
      else if (strcmp(output,"raw") == 0) output_mode = Raw;
      break;
    case 'd':
      alsa_device = strdup(optarg);
      break;
    case 'n':
      stream_name = strdup(optarg);
      break;
    case 't':
      target_latency_ms = atoi(optarg);
      if (target_latency_ms < 0) show_usage(argv[0]);
      break;
    case 'l':
      max_latency_ms = atoi(optarg);
      if (max_latency_ms < 0) show_usage(argv[0]);
      break;
    case 'v':
      verbosity += 1;
      break;
    default:
      show_usage(argv[0]);
    }
  }
  if (optind < argc) {
    fprintf(stderr, "Expected argument after options\n");
    show_usage(argv[0]);
  }

  // Opportunistic call to renice us, so we can keep up under
  // higher load conditions. This may fail when run as non-root.
  setpriority(PRIO_PROCESS, 0, -11);

  // initialize output
  switch (output_mode) {
    case Pulseaudio:
#if PULSEAUDIO_ENABLE
      if (verbosity) fprintf(stderr, "Using Pulseaudio output\n");
      if (pulse_output_init(target_latency_ms, max_latency_ms, stream_name) != 0) {
        return 1;
      }
      output_send_fn = pulse_output_send;
#else
      fprintf(stderr, "%s compiled without Pulseaudio support. Aborting\n", argv[0]);
      return 1;
#endif
      break;
    case Alsa:
#if ALSA_ENABLE
      if (verbosity) fprintf(stderr, "Using ALSA output\n");
      if (alsa_output_init(target_latency_ms, alsa_device) != 0) {
        return 1;
      }
      output_send_fn = alsa_output_send;
#else
      fprintf(stderr, "%s compiled without ALSA support. Aborting\n", argv[0]);
      return 1;
#endif
      break;
    case Raw:
      if (verbosity) fprintf(stderr, "Using raw output\n");
      if (raw_output_init() != 0) {
        return 1;
      }
      output_send_fn = raw_output_send;
    default:
      break;
  }

  // initialize receiver
  switch (receiver_mode) {
    case SharedMem:
      if (verbosity) fprintf(stderr, "Starting IVSHMEM receiver\n");
      init_shmem(ivshmem_device);
      receiver_rcv_fn = rcv_shmem;
      break;
    case Unicast:
    case Multicast:
    default:
      if (verbosity) fprintf(stderr, "Starting %s receiver\n", receiver_mode == Unicast ? "unicast" : "multicast");
      init_network(receiver_mode, interface, port, multicast_group);
      receiver_rcv_fn = rcv_network;
      break;
  }


  for (;;) {
    receiver_rcv_fn(&receiver_data);
    if (output_send_fn(&receiver_data) != 0)
      return 1;
  }

};
