// Google BSD license http://code.google.com/google_bsd_license.html
// Copyright 2012 Google Inc. wrightt@google.com

//
// This "main" connects the debugger to our socket management backend.
//

#include <getopt.h>
#include <errno.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash_table.h"
#include "device_listener.h"
#include "ios_webkit_debug_proxy.h"
#include "port_config.h"
#include "socket_manager.h"
#include "webinspector.h"
#include "websocket.h"

struct iwdpm_struct {
  char *config;
  char *frontend;
  bool is_debug;

  pc_t pc;
  sm_t sm;
  iwdp_t iwdp;
};
typedef struct iwdpm_struct *iwdpm_t;
#ifdef WIN32
iwdpm_t iwdpm_new() {
  return (iwdpm_t) calloc(sizeof(struct iwdpm_struct), 1);
}
#else
iwdpm_t iwdpm_new();
#endif
void iwdpm_free(iwdpm_t self);

int iwdpm_configure(iwdpm_t self, int argc, char **argv);

void iwdpm_create_bridge(iwdpm_t self);

static int quit_flag = 0;

static void on_signal(int sig) {
  quit_flag++;
}

int main(int argc, char** argv) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  iwdpm_t self = iwdpm_new();
  int ret = iwdpm_configure(self, argc, argv);
  if (ret) {
    exit(ret > 0 ? ret : 0);
    return ret;
  }

  iwdpm_create_bridge(self);

  iwdp_t iwdp = self->iwdp;
  if (iwdp->start(iwdp)) {
    return -1;// TODO cleanup
  }

  sm_t sm = self->sm;
  while (!quit_flag) {
    if (sm->select(sm, 2) < 0) {
      ret = -1;
      break;
    }
  }
  sm->cleanup(sm);
  iwdpm_free(self);
  return ret;
}
//
// Connect ios_webkit_debug_proxy to socket_selector/etc:
//

int iwdpm_subscribe(iwdp_t iwdp) {
  return dl_connect(-1);
}
int iwdpm_attach(iwdp_t iwdp, const char *device_id, char **to_device_id,
    char **to_device_name) {
  return wi_connect(device_id, to_device_id, to_device_name, -1);
}
iwdp_status iwdpm_select_port(iwdp_t iwdp, const char *device_id,
    int *to_port, int *to_min_port, int *to_max_port) {
  iwdpm_t self = (iwdpm_t)iwdp->state;
  int ret = 0;
  // reparse every time, in case the file has changed
  int is_file = 0;
  if (!self->pc) {
    self->pc = pc_new();
    if (pc_add_line(self->pc, self->config, strlen(self->config))) {
      pc_clear(self->pc);
      pc_add_file(self->pc, self->config);
      is_file = 1;
    }
  }
  ret = pc_select_port(self->pc, device_id, to_port, to_min_port,to_max_port);
  if (is_file) {
    pc_free(self->pc);
    self->pc = NULL;
  }
  return (ret ? IWDP_ERROR : IWDP_SUCCESS);
}
int iwdpm_listen(iwdp_t iwdp, int port) {
  return sm_listen(port);
}
int iwdpm_connect(iwdp_t iwdp, const char *hostname, int port) {
  return sm_connect(hostname, port);
}
iwdp_status iwdpm_send(iwdp_t iwdp, int fd, const char *data, size_t length) {
  sm_t sm = ((iwdpm_t)iwdp->state)->sm;
  return sm_send(sm, fd, NULL, data, length);
}
iwdp_status iwdpm_add_fd(iwdp_t iwdp, int fd, void *value, bool is_server) {
  sm_t sm = ((iwdpm_t)iwdp->state)->sm;
  return sm->add_fd(sm, fd, value, is_server);
}
iwdp_status iwdpm_remove_fd(iwdp_t iwdp, int fd) {
  sm_t sm = ((iwdpm_t)iwdp->state)->sm;
  return sm->remove_fd(sm, fd);
}
sm_status iwdpm_on_accept(sm_t sm, int s_fd, void *s_value,
    int fd, void **to_value) {
  iwdp_t iwdp = ((iwdpm_t)sm->state)->iwdp;
  return iwdp->on_accept(iwdp, s_fd, s_value, fd, to_value);
}
sm_status iwdpm_on_sent(sm_t sm, int fd, void *value,
    const char *buf, ssize_t length) {
  return SM_SUCCESS;
}
sm_status iwdpm_on_recv(sm_t sm, int fd, void *value,
    const char *buf, ssize_t length) {
  iwdp_t iwdp = ((iwdpm_t)sm->state)->iwdp;
  return iwdp->on_recv(iwdp, fd, value, buf, length);
}
sm_status iwdpm_on_close(sm_t sm, int fd, void *value, bool is_server) {
  iwdp_t iwdp = ((iwdpm_t)sm->state)->iwdp;
  return iwdp->on_close(iwdp, fd, value, is_server);
}

void iwdpm_create_bridge(iwdpm_t self) {
  sm_t sm = sm_new(4096);
  iwdp_t iwdp = iwdp_new(self->frontend);
  if (!sm || !iwdp) {
    sm_free(sm);
    return;
  }
  self->sm = sm;
  self->iwdp = iwdp;
  iwdp->subscribe = iwdpm_subscribe;
  iwdp->attach = iwdpm_attach;
  iwdp->select_port = iwdpm_select_port;
  iwdp->listen = iwdpm_listen;
  iwdp->connect = iwdpm_connect;
  iwdp->send = iwdpm_send;
  iwdp->add_fd = iwdpm_add_fd;
  iwdp->remove_fd = iwdpm_remove_fd;
  iwdp->state = self;
  iwdp->is_debug = &self->is_debug;
  sm->on_accept = iwdpm_on_accept;
  sm->on_sent = iwdpm_on_sent;
  sm->on_recv = iwdpm_on_recv;
  sm->on_close = iwdpm_on_close;
  sm->state = self;
  sm->is_debug = &self->is_debug;
}


void iwdpm_free(iwdpm_t self) {
  if (self) {
    pc_free(self->pc);
    iwdp_free(self->iwdp);
    sm_free(self->sm);
    free(self->config);
    free(self->frontend);
    memset(self, 0, sizeof(struct iwdpm_struct));
    free(self);
  }
}

iwdpm_t iwdpm_new(int argc, char **argv, int *to_exit) {
  iwdpm_t self = (iwdpm_t)malloc(sizeof(struct iwdpm_struct));
  if (!self) {
    return NULL;
  }
  memset(self, 0, sizeof(struct iwdpm_struct));
  return self;
}

int iwdpm_configure(iwdpm_t self, int argc, char **argv) {

  static struct option longopts[] = {
    {"config", 1, NULL, 'c'},
    {"frontend", 1, NULL, 'f'},
    {"no-frontend", 0, NULL, 'F'},
    {"debug", 0, NULL, 'd'},
    {"help", 0, NULL, 'h'},

    {NULL, 0, NULL, 0}
  };
  const char *DEFAULT_CONFIG = "null:9221,:9222-9322";
  const char *DEFAULT_FRONTEND =
     "https://chrome-devtools-frontend.appspot.com/static/27.0.1453.93/devtools.html";

  self->config = strdup(DEFAULT_CONFIG);
  self->frontend = strdup(DEFAULT_FRONTEND);

  int ret = 0;
  while (!ret) {
    int c = getopt_long(argc, argv, "hc:f:Fd", longopts, (int *)0);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'h':
        ret = -1;
        break;
      case 'c':
        free(self->config);
        self->config = strdup(optarg);
        break;
      case 'f':
      case 'F':
        free(self->frontend);
        self->frontend = (c == 'f' ? strdup(optarg) : NULL);
        break;
      case 'd':
        self->is_debug = true;
        break;
      default:
        ret = 2;
        break;
    }
  }
  if (!ret && ((argc - optind) > 0)) {
    ret = 2;
  }

  if (ret) {
    char *name = strrchr(argv[0], '/');
    printf(
        "Usage: %s [OPTIONS]\n"
        "iOS WebKit Remote Debugging Protocol Proxy.\n"
        "\n"
        "  -c, --config CSV\t[UDID]:port[-port] config(s), defaults to:\n"
        "          %s\n"
        "        which will list devices (\"null:\") on port 9221 and assign\n"
        "        all other devices (\":\") to the next unused port in the\n"
        "        9222-9322 range, in the (somewhat random) order that the\n"
        "        devices are detected.\n"
        "        E.g. to track a single device with a specific port:\n"
        "          4ea8dd11e8c4fbc1a2deadbeefa0fd3bbbb268c7:9227\n"
        "        The value can be the path to a file in the above format.\n"
        "\n"
        "  -f, --frontend URL\tDevTools frontend UI path or URL, defaults to:\n"
        "          %s\n"
        "        E.g. to use a local WebKit checkout:\n"
        "          /usr/local/WebCore/inspector/front-end/inspector.html\n"
        "        E.g. to use a remote server:\n"
        "          http://foo.com:1234/bar/inspector.html\n"
        "        The value must end in \".html\"\n"
        "\n"
        "  --no-frontend \tDisable the DevTools frontend.\n"
        "\n"
        "  -d, --debug\t\tEnable debug output.\n"
        "  -h, --help\t\tPrint this usage information.\n"
        "\n", (name ? name + 1 : argv[0]), DEFAULT_CONFIG, DEFAULT_FRONTEND);
  }
  return ret;
}

