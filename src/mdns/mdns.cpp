/**
 * Real Time Protocol Music Instrument Digital Interface Daemon
 * Copyright (C) 2019 David Moreno Montero <dmoreno@coralbits.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "./exceptions.hpp"
#include "./logger.hpp"
#include "./mdns.hpp"
#include "./netutils.hpp"
#include "./poller.hpp"
#include "./stringpp.hpp"
#include "./utils.hpp"

#define DEBUG0(...)
const bool debug0 = false;
// Optionally
// #define DEBUG0 DEBUG

using namespace rtpmidid;
using namespace std::chrono_literals;

static rtpmidid::ip4_t route_get_ip_for_route(struct in_addr);
static rtpmidid::ip4_t guess_default_ip();

mdns::mdns(const std::string &default_ip) {
  ip4 = inet_addr(default_ip.c_str());

  if (!ip4)
    ip4 = guess_default_ip();

  {
    uint8_t ip[4];
    *((int32_t *)&ip) = ip4;
    DEBUG("Default IP is {}.{}.{}.{}.", ip[0], ip[1], ip[2], ip[3]);
  }

  socketfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socketfd < 0) {
    throw rtpmidid::exception("Can not open mDNS socket. Out of sockets?");
  }
  int c = 1;
  if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &c, sizeof(c)) < 0) {
    throw rtpmidid::exception(
        "Can not open mDNS socket. Address reuse denied? {}", strerror(errno));
  }
  c = 1;
  if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEPORT, &c, sizeof(c)) < 0) {
    throw rtpmidid::exception("Can not open mDNS socket. Port reuse denied? {}",
                              strerror(errno));
  }

  memset(&multicast_addr, 0, sizeof(multicast_addr));
  multicast_addr.sin_family = AF_INET;
  inet_aton("224.0.0.251", &multicast_addr.sin_addr);
  multicast_addr.sin_port = htons(5353);
  if (bind(socketfd, (const struct sockaddr *)&multicast_addr,
           sizeof(multicast_addr)) < 0) {
    throw rtpmidid::exception(
        "Can not open mDNS socket. Maybe addres is in use?");
  }
  poller.add_fd_in(socketfd, [this](int) { this->mdns_ready(); });

  DEBUG("mDNS wating for requests at 224.0.0.251:5353");
}

mdns::~mdns() {
  // Could be more efficient on packet with all the announcements of EOF.
  for (auto &annp : announcements) {
    for (auto &srv : annp.second) {
      srv->ttl = 0;
      send_response(*srv);
    }
  }
}

bool read_question(mdns *server, parse_buffer_t &buffer) {
  uint8_t label[128];
  parse_buffer_t parse_label = {label, label + sizeof(label), label};

  read_label(buffer, parse_label);
  int type_ = buffer.read_uint16();
  UNUSED int class_ = buffer.read_uint16();
  DEBUG0("Question about: {} {} {}.", label, type_, class_);

  return server->answer_if_known(mdns::query_type_e(type_), (char *)label);
}

bool read_answer(mdns *server, parse_buffer_t &buffer) {
  uint8_t label[128];
  parse_buffer_t buffer_label = {label, label + 128, label};

  read_label(buffer, buffer_label);
  auto type_ = buffer.read_uint16();
  UNUSED auto class_ = buffer.read_uint16();

  auto ttl = buffer.read_uint32();

  auto data_length = buffer.read_uint16();
  auto *pos = buffer.position;

  if (type_ == mdns::PTR) { // PTR
    uint8_t answer[128];
    parse_buffer_t buffer_answer = {answer, answer + 128, answer};
    read_label(buffer, buffer_answer);
    // DEBUG("PTR Answer: {} {} {} -> <{}>", label, type_, class_, answer);
    mdns::service_ptr service((char *)label, ttl, (char *)answer);
    server->detected_service(&service);
  } else if (type_ == mdns::SRV) { // PTR
    // auto priority = read_uint16(data);
    buffer.position += 2;
    // auto weight = read_uint16(data);
    buffer.position += 2;
    buffer.assert_valid_position();
    auto port = buffer.read_uint16();

    uint8_t target[128];
    parse_buffer_t buffer_target = {target, target + 128, target};
    read_label(buffer, buffer_target);

    mdns::service_srv service((char *)label, ttl, (char *)target, port);
    server->detected_service(&service);

    // char answer[128];
    // len = read_label(data, end, buffer, answer, answer + sizeof(answer));
    // DEBUG("PTR Answer about: {} {} {} -> <{}>", label, type_, class_,
    // answer); DEBUG("Asking now about {} SRV", answer); server->query(answer,
    // mdns::SRV);
  } else if (type_ == mdns::A) {
    uint8_t ip[4] = {buffer.read_uint8(), buffer.read_uint8(),
                     buffer.read_uint8(), buffer.read_uint8()};
    mdns::service_a service((char *)label, ttl, ip);

    server->detected_service(&service);
  }
  buffer.position = pos + data_length;
  buffer.assert_valid_position();

  return true;
}

void mdns::on_discovery(const std::string &service, mdns::query_type_e qt,
                        std::function<void(const mdns::service *)> f) {
  if (service.length() > 100) {
    throw exception(
        "Service name too long. I only know how to search for smaller names.");
  }
  discovery_map[std::make_pair(qt, service)].push_back(f);
}

void mdns::remove_discovery(const std::string &service,
                            mdns::query_type_e type) {
  discovery_map.erase(std::make_pair(type, service));
}

void mdns::query(const std::string &service, mdns::query_type_e qt,
                 std::function<void(const mdns::service *)> f) {
  if (service.length() > 100) {
    throw exception(
        "Service name too long. I only know how to search for smaller names.");
  }
  query_map[std::make_pair(qt, service)].push_back(f);

  query(service, qt);
}

void mdns::announce(std::unique_ptr<service> service, bool broadcast) {
  if (service->label.length() > 100) {
    throw exception(
        "Cant announce a service this long. Max size is 100 chars.");
  }
  auto idx = std::make_pair(service->type, service->label);

  // preemptively tell everybody
  if (broadcast) {
    send_response(*service);
    INFO("Announce service: {}", service->to_string());
  } else {
    DEBUG("NOT announcing service: {}", service->to_string());
  }

  // Will reannounce acording to ttl. I keep a pointer, but if removed it will
  // be removed from the timers too.
  if (broadcast && service->ttl > 0) {
    reannounce_later(service.get());
  }

  // And store. This order to use service before storing.
  announcements[idx].push_back(std::move(service));
}

void mdns::reannounce_later(service *srv) {
  // DEBUG("Will reannounce in {}s", srv->ttl);
  auto timer_id = poller.add_timer_event(srv->ttl * 1s, [this, srv] {
    INFO("Reannounce srv: {}", srv->to_string());
    send_response(*srv);
    reannounce_later(srv);
  });
  srv->cache_timeout_id = std::move(timer_id);
}

void mdns::unannounce(service *srv) {
  srv->ttl = 0;
  send_response(*srv);

  auto idx = std::make_pair(srv->type, srv->label);
  auto annv = &announcements[idx];

  reannounce_timers.erase(srv);

  annv->erase(std::remove_if(annv->begin(), annv->end(),
                             [srv](const std::unique_ptr<service> &x) {
                               return x->equal(srv);
                             }),
              annv->end());
}

void mdns::send_response(const service &service) {
  uint8_t packet[1500];
  memset(packet, 0, 16); // Here there are some zeros, the rest will be
                         // overwritten or not used
  parse_buffer_t buffer = {packet, packet + 1500, packet};

  // Response and authoritative
  buffer.position[2] = 0x84;

  buffer.position += 6;
  // One answer
  buffer.write_uint16(1);

  // The query
  buffer.position = buffer.start + 12;
  write_label(buffer, service.label);

  // type
  buffer.write_uint16(service.type);
  // class IN
  buffer.write_uint16(1);
  // ttl
  buffer.write_uint32(service.ttl);
  // data_length. I prepare the spot
  auto length_data_pos = buffer.position;
  buffer.position += 2;
  switch (service.type) {
  case mdns::A: {
    auto a = static_cast<const mdns::service_a *>(&service);

    if (a->ip4 == 0) {
      // Put here my own IP
      buffer.write_uint32(htonl(ip4));
    } else {
      buffer.write_uint8(a->ip[0]);
      buffer.write_uint8(a->ip[1]);
      buffer.write_uint8(a->ip[2]);
      buffer.write_uint8(a->ip[3]);
    }
  } break;
  case mdns::PTR: {
    auto ptr = static_cast<const mdns::service_ptr *>(&service);
    write_label(buffer, ptr->servicename);
  } break;
  case mdns::SRV: {
    auto srv = static_cast<const mdns::service_srv *>(&service);
    buffer.write_uint16(0); // priority
    buffer.write_uint16(0); // weight
    buffer.write_uint16(srv->port);
    write_label(buffer, srv->hostname);
  }
  case mdns::TXT: {
    auto srv = static_cast<const mdns::service_txt *>(&service);
    write_label(buffer, srv->txt);
  } break;
  default:
    throw exception("I dont know how to announce this mDNS answer type: {}",
                    service.type);
  }

  uint16_t nbytes = buffer.position - length_data_pos - 2;
  // DEBUG("Send mDNS response: {}", service.to_string());

  // A little go and back
  raw_write_uint16(length_data_pos, nbytes);

  this->broadcast(&buffer);
  // sendto(socketfd, packet, buffer.length(), MSG_CONFIRM, (const struct
  // sockaddr *)&multicast_addr, sizeof(multicast_addr));
}

int mdns::broadcast(const parse_buffer_t *buffer) {
  return ::sendto(socketfd, buffer->start, buffer->capacity(), MSG_CONFIRM,
                  (const struct sockaddr *)&multicast_addr,
                  sizeof(multicast_addr));
}

bool mdns::answer_if_known(mdns::query_type_e type_, const std::string &label) {
  auto found = announcements.find(std::make_pair(type_, label));
  if (found != announcements.end()) {
    for (auto &response : found->second) {
      send_response(*response);
    }
    return true;
  }
  return false;
}

void mdns::query(const std::string &name, mdns::query_type_e type) {
  // Before asking, check my cache, and if there, use as answer
  auto at_cache = cache.find(std::make_pair(type, name));
  if (at_cache != cache.end()) {
    for (auto &service : at_cache->second) {
      detected_service(service.get());
    }
    return;
  }

  // Now I will ask for it
  // I will prepare the package here
  uint8_t packet[120];
  // transaction id. always 0 for mDNS
  memset(packet, 0, sizeof(packet));
  // I will only set what I need.
  packet[5] = 1;
  // Now the query itself
  parse_buffer_t buffer = {packet, packet + 120, packet + 12};
  write_label(buffer, name);
  // type ptr
  buffer.write_uint16(type);
  // query
  buffer.write_uint16(1);

  /// DONE
  if (debug0) {
    DEBUG("Packet ready! {} bytes", buffer.capacity());
    buffer.print_hex();
  }
  // DEBUG("Send query {} {}", name, type);
  broadcast(&buffer);
}

void parse_packet(mdns *mdns, parse_buffer_t &parse_buffer) {
  UNUSED int tid = parse_buffer.read_uint16();
  UNUSED int8_t flags = parse_buffer.read_uint16();
  // UNUSED bool is_query = !(flags & 0x8000);
  // UNUSED int opcode = (flags >> 11) & 0x0007;
  auto nquestions = parse_buffer.read_uint16();
  auto nanswers = parse_buffer.read_uint16();
  UNUSED auto nauthority = parse_buffer.read_uint16();
  UNUSED auto nadditional = parse_buffer.read_uint16();

  DEBUG0("mDNS packet: id: {}, is_query: {}, opcode: {}, nquestions: {}, "
         "nanswers: {}, nauthority: {}, nadditional: {}",
         tid, is_query ? "true" : "false", opcode, nquestions, nanswers,
         nauthority, nadditional);
  uint32_t i;
  for (i = 0; i < nquestions; i++) {
    auto ok = read_question(mdns, parse_buffer);
    if (!ok) {
      DEBUG0("Ignoring mDNS question!");
      return;
    }
  }
  for (i = 0; i < nanswers; i++) {
    auto ok = read_answer(mdns, parse_buffer);
    if (!ok) {
      DEBUG0("Ignoring mDNS answer!");
      return;
    }
  }
}

void mdns::mdns_ready() {
  uint8_t buffer[1501];
  memset(buffer, 0, sizeof(buffer));
  struct sockaddr_in cliaddr;
  unsigned int len = sizeof(cliaddr);
  auto read_length = recvfrom(socketfd, buffer, 1500, MSG_DONTWAIT,
                              (struct sockaddr *)&cliaddr, &len);

  uint32_t ip = route_get_ip_for_route(cliaddr.sin_addr);

  // memcpy(&ip4, (char*)&ip, sizeof(ip)); // Next line is equivalent, but
  // faster
  *((int32_t *)&ip4) = ip;
  // DEBUG("Response to {}.{}.{}.{}", ip4[0], ip4[1], ip4[2], ip4[3]);

  // char *remotename = inet_ntoa(cliaddr.sin_addr);
  // auto remoteport = ntohs(cliaddr.sin_port);
  // DEBUG("Got packet from {}:{}", remotename, remoteport);

  if (read_length < 16) {
    ERROR("Invalid mDNS packet. Minimum size is 16 bytes. Ignoring.");
    return;
  }

  parse_buffer_t parse_buffer{
      buffer,
      buffer + read_length,
      buffer,
  };

  if (debug0) {
    DEBUG("Got some data from mDNS: {}", read_length);
    parse_buffer.print_hex(true);
  }
  if (read_length > 1500) {
    ERROR("This mDNS implementation is not prepared for packages longer than "
          "1500 bytes. Please fill a bug report. Ignoring package. Actually we "
          "never read more. FILL A BUG REPORT.");
    return;
  }

  parse_packet(this, parse_buffer);
}

bool discovery_match(const std::string &expr, const std::string &label) {
  if (startswith(expr, "*.")) {
    return endswith(label, expr.substr(2));
  } else {
    return expr == label;
  }
}

void mdns::detected_service(const mdns::service *service) {
  auto type_label = std::make_pair(service->type, service->label);

  // Ignore my own records. Only if the exact match. Must check all one by one.
  for (auto &d : announcements) {
    if (d.first.first == service->type) {
      for (auto &s : d.second) {
        // DEBUG("Check if same {} =? {} => {}", service->to_string(),
        // s->to_string(), s->equal(service));
        if (s->equal(service)) {
          // DEBUG("Got my own announcement. Ignore.");
          return;
        }
      }
    }
  }

  // This has to be a gather what to call, and then call as f can try to remove
  // a service from discovery map. And that is not allowed (for loop and remove
  // item from the loop).
  std::vector<std::function<void(const mdns::service *)>> tocall;
  for (auto &discovery : discovery_map) {
    if (service->type == discovery.first.first &&
        discovery_match(discovery.first.second, service->label)) {
      for (auto &f : discovery.second) {
        tocall.push_back(f);
      }
    }
  }
  for (auto &f : tocall) {
    f(service);
  }
  for (auto &f : query_map[type_label]) {
    f(service);
  }

  // remove them from query map, as fulfilled
  query_map.erase(type_label);

  update_cache(service);
}

void mdns::update_cache(const mdns::service *service) {
  // Maybe add it to cache, update TTL, or remove, depends on TTL
  auto type_label = std::make_pair(service->type, service->label);

  // Remove from cache and timers
  if (service->ttl == 0) {

    auto &services = cache[type_label];
    if (services.size() == 0)
      return;
    services.erase(
        std::remove_if(std::begin(services), std::end(services),
                       [service](auto &d) { return d->equal(service); }));

    return;
  }

  // Will have to update or add
  mdns::service *service_at_cache = nullptr;
  for (auto &d : cache[type_label]) {
    // DEBUG("Check if equal \n\t\t{}\n\t\t{}", d->to_string(),
    // service->to_string());
    if (d->equal(service)) {
      // DEBUG("Got it at cache. Update TTL.");
      d->ttl = service->ttl;
      service_at_cache = d.get();
    }
  }
  if (!service_at_cache) {
    auto cloned = service->clone();
    cache[type_label].push_back(std::move(cloned));
  }
}

std::string mdns::local() {
  char hostname[256];
  gethostname(hostname, sizeof(hostname));
  return fmt::format("{}.local", hostname);
}

std::string std::to_string(const rtpmidid::mdns::service_ptr &s) {
  return fmt::format("PTR record. label: {}, pointer: {}", s.label,
                     s.servicename);
}
std::string std::to_string(const rtpmidid::mdns::service_a &s) {
  return fmt::format("A record. label: {}, ip: {}.{}.{}.{}", s.label,
                     uint8_t(s.ip[0]), uint8_t(s.ip[1]), uint8_t(s.ip[2]),
                     uint8_t(s.ip[3]));
}
std::string std::to_string(const rtpmidid::mdns::service_srv &s) {
  return fmt::format("SRV record. label: {}, hostname: {}, port: {}", s.label,
                     s.hostname, s.port);
}

/** Parses the /proc/net/routes to get the right address for a given IP, or
 * default route address **/

struct ip_route4_t {
  rtpmidid::ip4_t ip;
  rtpmidid::ip4_t mask;

  bool match(uint32_t other) { return (other & mask) == (ip & mask); }
};

static std::vector<ip_route4_t> routes;

static rtpmidid::ip4_t guess_default_ip() {
  ip_route4_t route;
  struct ifaddrs *addrs, *next;
  uint32_t route_default_ip = 0;

  getifaddrs(&addrs);

  next = addrs;
  while (next) {
    if (next->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in *ip = (struct sockaddr_in *)next->ifa_addr;
      struct sockaddr_in *ip_mask = (struct sockaddr_in *)next->ifa_netmask;

      route.ip = ip->sin_addr.s_addr;
      route.mask = ip_mask->sin_addr.s_addr;

      routes.push_back(route);

      if (route_default_ip == 0 && !(next->ifa_flags & IFF_LOOPBACK)) {
        // DEBUG("Add {:X} / {:X}", route.ip, route.mask);
        route_default_ip = route.ip;
      } else {
        // DEBUG("Skip Add {:X} / {:X}", route.ip, route.mask);
      }
    }

    next = next->ifa_next;
  }

  freeifaddrs(addrs);

  return route_default_ip;
}

static rtpmidid::ip4_t route_get_ip_for_route(struct in_addr in_other) {
  union ip_t {
    uint8_t ip4p[4];
    rtpmidid::ip4_t ip4;
  };
  ip_t other;
  other.ip4 = in_other.s_addr;

  for (auto &route : routes) {
    if (route.match(other.ip4)) {
      // DEBUG("Found at IP routes: {}", route.ip);
      return route.ip;
    }
  }
  // DEBUG("Default");
  return 0;
}
