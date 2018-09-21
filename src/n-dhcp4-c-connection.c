/*
 * DHCPv4 Client Connection
 *
 * XXX
 */

#include <assert.h>
#include <errno.h>
#include <net/if_arp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include "n-dhcp4-private.h"
#include "util/packet.h"

enum {
        N_DHCP4_CONNECTION_STATE_INIT,
        N_DHCP4_CONNECTION_STATE_PACKET,
        N_DHCP4_CONNECTION_STATE_DRAINING,
        N_DHCP4_CONNECTION_STATE_UDP,
};

int n_dhcp4_c_connection_init(NDhcp4CConnection *connection, int ifindex, uint8_t htype,
                              uint8_t hlen, const uint8_t *chaddr, const uint8_t *bhaddr,
                              size_t idlen, const uint8_t *id,
                              bool request_broadcast) {
        if (hlen > sizeof(connection->chaddr))
                return -EINVAL;
        if (idlen == 1)
                return -EINVAL;

        connection->ifindex = ifindex;
        connection->htype = htype;
        connection->hlen = hlen;
        connection->request_broadcast = request_broadcast;
        memcpy(connection->bhaddr, bhaddr, hlen);
        memcpy(connection->chaddr, chaddr, hlen);
        memcpy(connection->id, id, idlen);

        if (htype == ARPHRD_INFINIBAND)
                connection->request_broadcast = true;
        else
                connection->send_chaddr = true;

        return 0;
}

void n_dhcp4_c_connection_deinit(NDhcp4CConnection *connection) {
        if (*connection->efdp >= 0) {
                if (connection->ufd >= 0) {
                        epoll_ctl(*connection->efdp, EPOLL_CTL_DEL, connection->ufd, NULL);
                        close(connection->ufd);
                }

                if (connection->pfd >= 0) {
                        epoll_ctl(*connection->efdp, EPOLL_CTL_DEL, connection->pfd, NULL);
                        close(connection->pfd);
                }
        }

        *connection = (NDhcp4CConnection)N_DHCP4_C_CONNECTION_NULL(connection->efdp);
}

int n_dhcp4_c_connection_listen(NDhcp4CConnection *connection) {
        struct epoll_event ev = {
                .events = EPOLLIN,
        };
        int r;

        assert(connection->state == N_DHCP4_CONNECTION_STATE_INIT);

        r = n_dhcp4_network_client_packet_socket_new(&connection->pfd, connection->ifindex);
        if (r < 0)
                return r;

        ev.data.u32 = N_DHCP4_CLIENT_EPOLL_CONNECTION;
        r = epoll_ctl(*connection->efdp, EPOLL_CTL_ADD, connection->pfd, &ev);
        if (r < 0)
                return -errno;

        connection->state = N_DHCP4_CONNECTION_STATE_PACKET;

        return 0;
}

int n_dhcp4_c_connection_connect(NDhcp4CConnection *connection, const struct in_addr *client, const struct in_addr *server) {
        struct epoll_event ev = {
                .events = EPOLLIN,
        };
        int r;

        assert(connection->state == N_DHCP4_CONNECTION_STATE_PACKET);

        r = n_dhcp4_network_client_udp_socket_new(&connection->ufd, connection->ifindex, client, server);
        if (r < 0)
                return r;

        ev.data.u32 = N_DHCP4_CLIENT_EPOLL_CONNECTION;
        r = epoll_ctl(*connection->efdp, EPOLL_CTL_ADD, connection->pfd, &ev);
        if (r < 0)
                return -errno;

        r = packet_shutdown(connection->pfd);
        if (r < 0)
                return r;

        connection->ciaddr = client->s_addr;
        connection->siaddr = server->s_addr;
        connection->state = N_DHCP4_CONNECTION_STATE_DRAINING;

        return 0;
}

static int n_dhcp4_c_connection_verify_incoming(NDhcp4CConnection *connection, NDhcp4Incoming *message) {
        NDhcp4Header *header = n_dhcp4_incoming_get_header(message);
        const void *id;
        size_t idlen;
        int r;

        if (memcmp(connection->chaddr, header->chaddr, connection->hlen) != 0)
                return -EINVAL;

        r = n_dhcp4_incoming_query(message, N_DHCP4_OPTION_CLIENT_IDENTIFIER, &id, &idlen);
        if (r == -ENODATA) {
                id = NULL;
                idlen = 0;
        } else if (r < 0) {
                return r;
        }

        if (idlen != connection->idlen)
                return -EINVAL;

        if (memcmp(connection->id, id, idlen) != 0)
                return -EINVAL;

        return 0;
}

static int n_dhcp4_c_connection_dispatch_packet(NDhcp4CConnection *connection, NDhcp4Incoming **messagep) {
        uint8_t buf[1 << 16];
        ssize_t len;
        int r;

        len = packet_recv_udp(connection->pfd, buf, sizeof(buf), 0);
        if (len == 0) {
                *messagep = NULL;
                return 0;
        } else if (len < 0) {
                return -errno;
        }

        /* XXX: handle malformed packets gracefully */
        r = n_dhcp4_incoming_new(messagep, buf, len);
        if (r < 0)
                return r;

        return 0;
}

static int n_dhcp4_c_connection_dispatch_udp(NDhcp4CConnection *connection, NDhcp4Incoming **messagep) {
        uint8_t buf[1 << 16];
        ssize_t len;
        int r;

        len = recv(connection->ufd, buf, sizeof(buf), 0);
        if (len == 0) {
                *messagep = NULL;
                return 0;
        } else if (len < 0) {
                return -errno;
        }

        /* XXX: handle malformed packets gracefully */
        r = n_dhcp4_incoming_new(messagep, buf, len);
        if (r < 0)
                return r;

        return 0;
}

int n_dhcp4_c_connection_dispatch(NDhcp4CConnection *connection, NDhcp4Incoming **messagep) {
        _cleanup_(n_dhcp4_incoming_freep) NDhcp4Incoming *message = NULL;
        int r;

        switch (connection->state) {
        case N_DHCP4_CONNECTION_STATE_PACKET:
                r = n_dhcp4_c_connection_dispatch_packet(connection, &message);
                if (r < 0)
                        return r;

                break;
        case N_DHCP4_CONNECTION_STATE_DRAINING:
                r = n_dhcp4_c_connection_dispatch_packet(connection, &message);
                if (r >= 0)
                        break;
                else if (r != -EAGAIN)
                        return r;

                /*
                 * The UDP socket is open and the packet socket has been shut down
                 * and drained, clean up the packet socket and fall through to
                 * dispatching the UDP socket.
                 */
                epoll_ctl(*connection->efdp, EPOLL_CTL_DEL, connection->pfd, NULL);
                close(connection->pfd);
                connection->state = N_DHCP4_CONNECTION_STATE_UDP;

                /* fall-through */
        case N_DHCP4_CONNECTION_STATE_UDP:
                r = n_dhcp4_c_connection_dispatch_udp(connection, &message);
                if (r < 0)
                        return r;
        }

        r = n_dhcp4_c_connection_verify_incoming(connection, message);
        if (r < 0)
                return 0;

        *messagep = message;
        message = NULL;
        return 0;
}

static int n_dhcp4_c_connection_packet_broadcast(NDhcp4CConnection *connection, NDhcp4Outgoing *message) {
        const void *buf;
        size_t n_buf;
        int r;

        assert(connection->state == N_DHCP4_CONNECTION_STATE_PACKET);

        n_buf = n_dhcp4_outgoing_get_raw(message, &buf);

        r = n_dhcp4_network_client_packet_send(connection->pfd, connection->ifindex,
                                               connection->bhaddr, connection->hlen,
                                               buf, n_buf);
        if (r < 0)
                return r;

        return 0;
}

static int n_dhcp4_c_connection_udp_broadcast(NDhcp4CConnection *connection, NDhcp4Outgoing *message) {
        const void *buf;
        size_t n_buf;
        int r;

        assert(connection->state > N_DHCP4_CONNECTION_STATE_PACKET);

        n_buf = n_dhcp4_outgoing_get_raw(message, &buf);

        r = n_dhcp4_network_client_udp_broadcast(connection->ufd, buf, n_buf);
        if (r < 0)
                return r;

        return 0;
}

static int n_dhcp4_c_connection_udp_send(NDhcp4CConnection *connection, NDhcp4Outgoing *message) {
        const void *buf;
        size_t n_buf;
        int r;

        assert(connection->state > N_DHCP4_CONNECTION_STATE_PACKET);

        n_buf = n_dhcp4_outgoing_get_raw(message, &buf);

        r = n_dhcp4_network_client_udp_send(connection->ufd, buf, n_buf);
        if (r < 0)
                return r;

        return 0;
}

static void n_dhcp4_c_connection_init_header(NDhcp4CConnection *connection, NDhcp4Header *header) {
        header->op = N_DHCP4_OP_BOOTREQUEST;
        header->htype = connection->htype;
        header->ciaddr = connection->ciaddr;

        if (connection->request_broadcast)
                header->flags |= N_DHCP4_MESSAGE_FLAG_BROADCAST;

        if (connection->send_chaddr) {
                assert(connection->hlen <= sizeof(header->chaddr));

                header->hlen = connection->hlen;
                memcpy(header->chaddr, connection->chaddr, connection->hlen);
        }
}

static int n_dhcp4_c_connection_new_message(NDhcp4CConnection *connection, NDhcp4Outgoing **messagep, uint8_t type) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        NDhcp4Header *header;
        int r;

        r = n_dhcp4_outgoing_new(&message, 0, N_DHCP4_OVERLOAD_FILE | N_DHCP4_OVERLOAD_SNAME);
        if (r < 0)
                return r;

        header = n_dhcp4_outgoing_get_header(message);
        n_dhcp4_c_connection_init_header(connection, header);

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_MESSAGE_TYPE, &type, sizeof(type));
        if (r < 0)
                return r;

        if (connection->idlen) {
                r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_CLIENT_IDENTIFIER, connection->id, connection->idlen);
                if (r < 0)
                        return r;
        }

        switch (type) {
        case N_DHCP4_MESSAGE_DISCOVER:
        case N_DHCP4_MESSAGE_REQUEST:
        case N_DHCP4_MESSAGE_INFORM:
                if (connection->state <= N_DHCP4_CONNECTION_STATE_PACKET) {
                        if (connection->mtu > 0) {
                                uint16_t mtu = htons(connection->mtu);

                                r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_MAXIMUM_MESSAGE_SIZE, &mtu, sizeof(mtu));
                                if (r < 0)
                                        return r;
                        }
                } else {
                        uint16_t mtu = htons(N_DHCP4_NETWORK_UDP_MAX_SIZE);

                        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_MAXIMUM_MESSAGE_SIZE, &mtu, sizeof(mtu));
                        if (r < 0)
                                return r;
                }
                break;
        default:
                break;
        }

        *messagep = message;
        message = NULL;
        return 0;
}

static void n_dhcp4_c_connection_outgoing_set_xid(NDhcp4Outgoing *message, uint32_t xid, uint32_t secs) {
        NDhcp4Header *header = n_dhcp4_outgoing_get_header(message);

        /*
         * Some DHCP servers will reject DISCOVER or REQUEST messages if 'secs' is
         * not est.
         */
        assert(secs != 0);

        header->secs = htonl(secs);
        header->xid = xid;
}

/*
 *      RFC2131 3.1
 *
 *      The client broadcasts a DHCPDISCOVER message on its local physical
 *      subnet.  The DHCPDISCOVER message MAY include options that suggest
 *      values for the network address and lease duration.  BOOTP relay
 *      agents may pass the message on to DHCP servers not on the same
 *      physical subnet.
 *
 *      RFC2131 3.5
 *
 *      [...] in its initial DHCPDISCOVER or DHCPREQUEST message, a client
 *      may provide the server with a list of specific parameters the
 *      client is interested in.  If the client includes a list of
 *      parameters in a DHCPDISCOVER message, it MUST include that list in
 *      any subsequent DHCPREQUEST messages.
 *
 *      [...]
 *
 *      In addition, the client may suggest values for the network address
 *      and lease time in the DHCPDISCOVER message.  The client may include
 *      the 'requested IP address' option to suggest that a particular IP
 *      address be assigned, and may include the 'IP address lease time'
 *      option to suggest the lease time it would like.  Other options
 *      representing "hints" at configuration parameters are allowed in a
 *      DHCPDISCOVER or DHCPREQUEST message.
 *
 *      RFC2131 4.4.1
 *
 *      The client generates and records a random transaction identifier and
 *      inserts that identifier into the 'xid' field.  The client records its
 *      own local time for later use in computing the lease expiration.  The
 *      client then broadcasts the DHCPDISCOVER on the local hardware
 *      broadcast address to the 0xffffffff IP broadcast address and 'DHCP
 *      server' UDP port.
 *
 *      If the 'xid' of an arriving DHCPOFFER message does not match the
 *      'xid' of the most recent DHCPDISCOVER message, the DHCPOFFER message
 *      must be silently discarded.  Any arriving DHCPACK messages must be
 *      silently discarded.
 */
int n_dhcp4_c_connection_discover(NDhcp4CConnection *connection, uint32_t xid, uint32_t secs) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_DISCOVER);
        if (r < 0)
                return r;

        n_dhcp4_c_connection_outgoing_set_xid(message, xid, secs);

        r = n_dhcp4_c_connection_packet_broadcast(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 4.3.2
 *
 *      Client inserts the address of the selected server in 'server
 *      identifier', 'ciaddr' MUST be zero, 'requested IP address' MUST be
 *      filled in with the yiaddr value from the chosen DHCPOFFER.
 */
int n_dhcp4_c_connection_select(NDhcp4CConnection *connection, const struct in_addr *client, const struct in_addr *server, uint32_t xid, uint32_t secs) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_REQUEST);
        if (r < 0)
                return r;

        n_dhcp4_c_connection_outgoing_set_xid(message, xid, secs);

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_REQUESTED_IP_ADDRESS, client, sizeof(*client));
        if (r < 0)
                return r;

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_SERVER_IDENTIFIER, server, sizeof(*server));
        if (r < 0)
                return r;

        r = n_dhcp4_c_connection_packet_broadcast(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 4.3.2
 *
 *      'server identifier' MUST NOT be filled in, 'requested IP address'
 *      option MUST be filled in with client's notion of its previously
 *      assigned address. 'ciaddr' MUST be zero. The client is seeking to
 *      verify a previously allocated, cached configuration. Server SHOULD
 *      send a DHCPNAK message to the client if the 'requested IP address'
 *      is incorrect, or is on the wrong network.
 */
int n_dhcp4_c_connection_reboot(NDhcp4CConnection *connection, const struct in_addr *client, uint32_t xid, uint32_t secs) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_REQUEST);
        if (r < 0)
                return r;

        n_dhcp4_c_connection_outgoing_set_xid(message, xid, secs);

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_REQUESTED_IP_ADDRESS, client, sizeof(*client));
        if (r < 0)
                return r;

        r = n_dhcp4_c_connection_packet_broadcast(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 4.3.2
 *
 *      'server identifier' MUST NOT be filled in, 'requested IP address'
 *      option MUST NOT be filled in, 'ciaddr' MUST be filled in with
 *      client's IP address. In this situation, the client is completely
 *      configured, and is trying to extend its lease. This message will
 *      be unicast, so no relay agents will be involved in its
 *      transmission.  Because 'giaddr' is therefore not filled in, the
 *      DHCP server will trust the value in 'ciaddr', and use it when
 *      replying to the client.
 *
 *      A client MAY choose to renew or extend its lease prior to T1.  The
 *      server may choose not to extend the lease (as a policy decision by
 *      the network administrator), but should return a DHCPACK message
 *      regardless.
 *
 *      RFC2131 4.4.5
 *
 *      At time T1 the client moves to RENEWING state and sends (via unicast)
 *      a DHCPREQUEST message to the server to extend its lease.  The client
 *      sets the 'ciaddr' field in the DHCPREQUEST to its current network
 *      address. The client records the local time at which the DHCPREQUEST
 *      message is sent for computation of the lease expiration time.  The
 *      client MUST NOT include a 'server identifier' in the DHCPREQUEST
 *      message.
 */
int n_dhcp4_c_connection_renew(NDhcp4CConnection *connection, uint32_t xid, uint32_t secs) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_REQUEST);
        if (r < 0)
                return r;

        n_dhcp4_c_connection_outgoing_set_xid(message, xid, secs);

        r = n_dhcp4_c_connection_udp_send(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 4.3.2
 *
 *      'server identifier' MUST NOT be filled in, 'requested IP address'
 *      option MUST NOT be filled in, 'ciaddr' MUST be filled in with
 *      client's IP address. In this situation, the client is completely
 *      configured, and is trying to extend its lease. This message MUST
 *      be broadcast to the 0xffffffff IP broadcast address.  The DHCP
 *      server SHOULD check 'ciaddr' for correctness before replying to
 *      the DHCPREQUEST.
 *
 *      RFC2131 4.4.5
 *
 *      If no DHCPACK arrives before time T2, the client moves to REBINDING
 *      state and sends (via broadcast) a DHCPREQUEST message to extend its
 *      lease.  The client sets the 'ciaddr' field in the DHCPREQUEST to its
 *      current network address.  The client MUST NOT include a 'server
 *      identifier' in the DHCPREQUEST message.
 */
int n_dhcp4_c_connection_rebind(NDhcp4CConnection *connection, uint32_t xid, uint32_t secs) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_REQUEST);
        if (r < 0)
                return r;

        n_dhcp4_c_connection_outgoing_set_xid(message, xid, secs);

        r = n_dhcp4_c_connection_udp_broadcast(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 3.2
 *
 *      If the client detects that the IP address in the DHCPACK message
 *      is already in use, the client MUST send a DHCPDECLINE message to the
 *      server and restarts the configuration process by requesting a
 *      new network address.
 *
 *      RFC2131 4.4.4
 *
 *      Because the client is declining the use of the IP address supplied by
 *      the server, the client broadcasts DHCPDECLINE messages.
 */
int n_dhcp4_c_connection_decline(NDhcp4CConnection *connection, const char *error, const struct in_addr *client, const struct in_addr *server) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_DECLINE);
        if (r < 0)
                return r;

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_REQUESTED_IP_ADDRESS, client, sizeof(*client));
        if (r < 0)
                return r;

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_SERVER_IDENTIFIER, server, sizeof(*server));
        if (r < 0)
                return r;

        if (error) {
                r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_ERROR_MESSAGE, error, strlen(error) + 1);
                if (r < 0)
                        return r;
        }

        r = n_dhcp4_c_connection_packet_broadcast(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 3.4
 *
 *      If a client has obtained a network address through some other means
 *      (e.g., manual configuration), it may use a DHCPINFORM request message
 *      to obtain other local configuration parameters.
 *
 *      RFC2131 4.4
 *
 *      The DHCPINFORM message is not shown in figure 5.  A client simply
 *      sends the DHCPINFORM and waits for DHCPACK messages.  Once the client
 *      has selected its parameters, it has completed the configuration
 *      process.
 *
 *      RFC2131 4.4.3
 *
 *      The client sends a DHCPINFORM message. The client may request
 *      specific configuration parameters by including the 'parameter request
 *      list' option. The client generates and records a random transaction
 *      identifier and inserts that identifier into the 'xid' field. The
 *      client places its own network address in the 'ciaddr' field. The
 *      client SHOULD NOT request lease time parameters.
 *
 *      The client then unicasts the DHCPINFORM to the DHCP server if it
 *      knows the server's address, otherwise it broadcasts the message to
 *      the limited (all 1s) broadcast address.  DHCPINFORM messages MUST be
 *      directed to the 'DHCP server' UDP port.
 */
int n_dhcp4_c_connection_inform(NDhcp4CConnection *connection, uint32_t xid, uint32_t secs) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_INFORM);
        if (r < 0)
                return r;

        n_dhcp4_c_connection_outgoing_set_xid(message, xid, secs);

        r = n_dhcp4_c_connection_udp_broadcast(connection, message);
        if (r < 0)
                return r;

        return 0;
}

/*
 *      RFC2131 3.1
 *
 *      The client may choose to relinquish its lease on a network address
 *      by sending a DHCPRELEASE message to the server.  The client
 *      identifies the lease to be released with its 'client identifier',
 *      or 'chaddr' and network address in the DHCPRELEASE message. If the
 *      client used a 'client identifier' when it obtained the lease, it
 *      MUST use the same 'client identifier' in the DHCPRELEASE message.
 *
 *      RFC2131 3.2
 *
 *      The client may choose to relinquish its lease on a network
 *      address by sending a DHCPRELEASE message to the server.  The
 *      client identifies the lease to be released with its
 *      'client identifier', or 'chaddr' and network address in the
 *      DHCPRELEASE message.
 *
 *      Note that in this case, where the client retains its network
 *      address locally, the client will not normally relinquish its
 *      lease during a graceful shutdown.  Only in the case where the
 *      client explicitly needs to relinquish its lease, e.g., the client
 *      is about to be moved to a different subnet, will the client send
 *      a DHCPRELEASE message.
 *
 *      RFC2131 4.4.4
 *
 *      The client unicasts DHCPRELEASE messages to the server.
 *
 *      RFC2131 4.4.6
 *
 *      If the client no longer requires use of its assigned network address
 *      (e.g., the client is gracefully shut down), the client sends a
 *      DHCPRELEASE message to the server.  Note that the correct operation
 *      of DHCP does not depend on the transmission of DHCPRELEASE messages.
 */
int n_dhcp4_c_connection_release(NDhcp4CConnection *connection, const char *error) {
        _cleanup_(n_dhcp4_outgoing_freep) NDhcp4Outgoing *message = NULL;
        int r;

        r = n_dhcp4_c_connection_new_message(connection, &message, N_DHCP4_MESSAGE_RELEASE);
        if (r < 0)
                return r;

        r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_SERVER_IDENTIFIER, &connection->siaddr, sizeof(connection->siaddr));
        if (r < 0)
                return r;

        if (error) {
                r = n_dhcp4_outgoing_append(message, N_DHCP4_OPTION_ERROR_MESSAGE, error, strlen(error) + 1);
                if (r < 0)
                        return r;
        }

        r = n_dhcp4_c_connection_udp_send(connection, message);
        if (r < 0)
                return r;

        return 0;
}
