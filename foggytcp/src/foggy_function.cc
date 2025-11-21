/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120) 
course taught at Hong Kong University of Science and Technology. 

No part of the project may be copied and/or distributed without 
the express permission of the course staff. Everyone is prohibited 
from releasing their forks in any public places. */

#include <deque>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sys/time.h>

#include "foggy_function.h"
#include "foggy_backend.h"


#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

#define DEBUG_PRINT 1
#define debug_printf(fmt, ...)                            \
  do {                                                    \
    if (DEBUG_PRINT) fprintf(stdout, fmt, ##__VA_ARGS__); \
  } while (0)

#define TIMEOUT_MS 3000  // 3 seconds timeout


/**
 * Updates the socket information to represent the newly received packet.
 *
 * In the current stop-and-wait implementation, this function also sends an
 * acknowledgement for the packet.
 *
 * @param sock The socket used for handling packets received.
 * @param pkt The packet data received by the socket.
 */
void on_recv_pkt(foggy_socket_t *sock, uint8_t *pkt) {
  debug_printf("Received packet\n");
  foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)pkt;
  uint8_t flags = get_flags(hdr);

  switch (flags) {
    case ACK_FLAG_MASK: {
      uint32_t ack = get_ack(hdr);
      printf("Receive ACK %d\n", ack);

      sock->window.advertised_window = get_advertised_window(hdr);

      // Handle congestion control when receiving ACK
      pthread_mutex_lock(&(sock->window.ack_lock));
      
      if (after(ack, sock->window.last_ack_received)) {
        // Calculate RTT and update base_rtt
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double rtt = -1.0;
        
        for (auto it = sock->send_window.begin(); it != sock->send_window.end(); ++it) {
            foggy_tcp_header_t *h = (foggy_tcp_header_t *)it->msg;
            uint32_t seq = get_seq(h);
            uint16_t len = get_payload_len(it->msg);
            if (seq + len == ack) {
                if (it->retransmissions == 0) {
                    long elapsed_ms = (current_time.tv_sec - it->send_time.tv_sec) * 1000 +
                                      (current_time.tv_nsec - it->send_time.tv_nsec) / 1000000;
                    rtt = (double)elapsed_ms;
                }
                break;
            }
        }

        if (rtt > 0) {
            if (rtt < sock->window.base_rtt) {
                sock->window.base_rtt = rtt;
            }
            
            // Vegas Logic
            double target_cwnd = (sock->window.congestion_window / rtt) * sock->window.base_rtt;
            double diff = sock->window.congestion_window - target_cwnd;
            
            double alpha = 2 * MSS;
            double beta = 4 * MSS;
            
            if (sock->window.reno_state == RENO_SLOW_START) {
                 if (diff > MSS) {
                     sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
                     // Transition to CA logic immediately
                     if (diff < alpha) {
                        sock->window.congestion_window += (MSS * MSS) / sock->window.congestion_window;
                     } else if (diff > beta) {
                        sock->window.congestion_window -= (MSS * MSS) / sock->window.congestion_window;
                        if (sock->window.congestion_window < MSS) sock->window.congestion_window = MSS;
                     }
                 } else {
                     sock->window.congestion_window += MSS;
                 }
            } else if (sock->window.reno_state == RENO_CONGESTION_AVOIDANCE) {
                if (diff < alpha) {
                    sock->window.congestion_window += (MSS * MSS) / sock->window.congestion_window;
                } else if (diff > beta) {
                    sock->window.congestion_window -= (MSS * MSS) / sock->window.congestion_window;
                    if (sock->window.congestion_window < MSS) sock->window.congestion_window = MSS;
                }
            }
        } else {
            // Fallback to Reno-like behavior if RTT not available (e.g. retransmission)
            // Or just do nothing to be safe
             if (sock->window.reno_state == RENO_SLOW_START) {
                sock->window.congestion_window += MSS;
             } else if (sock->window.reno_state == RENO_CONGESTION_AVOIDANCE) {
                sock->window.congestion_window += (MSS * MSS) / sock->window.congestion_window;
             }
        }
        
        if (sock->window.reno_state == RENO_FAST_RECOVERY) {
          sock->window.congestion_window = sock->window.ssthresh;
          sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
        }
        
        sock->window.last_ack_received = ack;
        sock->window.dup_ack_count = 0;
      } else if (ack == sock->window.last_ack_received) {
        // Duplicate ACK
        sock->window.dup_ack_count++;
        
        if (sock->window.dup_ack_count == 3) {
          sock->window.ssthresh = MAX(sock->window.congestion_window / 2, MSS);
          sock->window.congestion_window = sock->window.ssthresh + 3 * MSS;
          sock->window.reno_state = RENO_FAST_RECOVERY;
          pthread_mutex_unlock(&(sock->window.ack_lock));
          for (auto it = sock->send_window.begin(); it != sock->send_window.end(); ++it) {
            if (it->is_sent) {
              foggy_tcp_header_t *hdr_temp = (foggy_tcp_header_t *)it->msg;
              if (get_seq(hdr_temp) == ack) {
                // Retransmit this packet
                sendto(sock->socket, it->msg, get_plen(hdr_temp), 0,
                       (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
                clock_gettime(CLOCK_MONOTONIC, &it->send_time);
                it->retransmissions++;
                break;
              }
            }
          }
          pthread_mutex_lock(&(sock->window.ack_lock));
        } else if (sock->window.reno_state == RENO_FAST_RECOVERY) {
          // Additional duplicate ACK in fast recovery
          // sock->window.congestion_window += MSS;
        }
      }
      
      pthread_mutex_unlock(&(sock->window.ack_lock));
    }

    // Fallthrough.
    default: {
      if (get_payload_len(pkt) > 0) {

        sock->window.advertised_window = get_advertised_window(hdr);
        // Add the packet to receive window and process receive window
        add_receive_window(sock, pkt);
        process_receive_window(sock);
        // Send ACK

        uint8_t *ack_pkt = create_packet(
            sock->my_port, ntohs(sock->conn.sin_port),
            sock->window.last_byte_sent, sock->window.next_seq_expected,
            sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t), ACK_FLAG_MASK,
            MAX(MAX_NETWORK_BUFFER - (uint32_t)sock->received_len, MSS), 0,
            NULL, NULL, 0);
        sendto(sock->socket, ack_pkt, sizeof(foggy_tcp_header_t), 0,
               (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
        free(ack_pkt);
      }
    }
  }
}

/**
 * Breaks up the data into packets and sends a single packet at a time.
 *
 * You should most certainly update this function in your implementation.
 *
 * @param sock The socket to use for sending data.
 * @param data The data to be sent.
 * @param buf_len The length of the data being sent.
 */
void send_pkts(foggy_socket_t *sock, uint8_t *data, int buf_len) {
  uint8_t *data_offset = data;
  
  // Add new data to the send window
  if (buf_len > 0) {
    while (buf_len != 0) {
      uint16_t payload_len = MIN(buf_len, (int)MSS);

      send_window_slot_t slot;
      slot.is_sent = 0;
      slot.is_rtt_sample = 0;
      slot.retransmissions = 0;
      slot.msg = create_packet(
          sock->my_port, ntohs(sock->conn.sin_port),
          sock->window.last_byte_sent, sock->window.next_seq_expected,
          sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t) + payload_len,
          ACK_FLAG_MASK,
          MAX(MAX_NETWORK_BUFFER - (uint32_t)sock->received_len, MSS), 0, NULL,
          data_offset, payload_len);
      sock->send_window.push_back(slot);

      buf_len -= payload_len;
      data_offset += payload_len;
      sock->window.last_byte_sent += payload_len;
    }
  }
  
  // Try to transmit packets from the send window
  transmit_send_window(sock);
  
  // Remove acknowledged packets
  receive_send_window(sock);
}


void add_receive_window(foggy_socket_t *sock, uint8_t *pkt) {
  foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)pkt;
  uint32_t seq = get_seq(hdr);
  uint16_t payload_len = get_payload_len(pkt);
  
  // Check if this is a duplicate or already received packet
  if (before(seq + payload_len, sock->window.next_seq_expected)) {
    return;
  }
  
  // Calculate the position in the receive window
  uint32_t offset = seq - sock->window.next_seq_expected;
  uint32_t slot_index = offset / MSS;
  
  if (slot_index >= RECEIVE_WINDOW_SLOT_SIZE) {
    return;
  }
  
  receive_window_slot_t *cur_slot = &(sock->receive_window[slot_index]);
  if (cur_slot->is_used == 0) {
    cur_slot->is_used = 1;
    cur_slot->msg = (uint8_t*) malloc(get_plen(hdr));
    memcpy(cur_slot->msg, pkt, get_plen(hdr));
  }
}

void process_receive_window(foggy_socket_t *sock) {
  // Process all consecutive packets starting from the first slot
  while (1) {
    receive_window_slot_t *cur_slot = &(sock->receive_window[0]);
    
    if (cur_slot->is_used == 0) {
      // No packet in the first slot, stop processing
      break;
    }
    
    foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)cur_slot->msg;
    
    // Check if this is the expected packet
    if (get_seq(hdr) != sock->window.next_seq_expected) {
      // Out of order packet, stop processing
      break;
    }
    
    // Process this packet
    uint16_t payload_len = get_payload_len(cur_slot->msg);
    sock->window.next_seq_expected += payload_len;
    
    // Copy to received_buf
    sock->received_buf = (uint8_t*)
        realloc(sock->received_buf, sock->received_len + payload_len);
    memcpy(sock->received_buf + sock->received_len, get_payload(cur_slot->msg),
           payload_len);
    sock->received_len += payload_len;
    
    // Free the slot
    cur_slot->is_used = 0;
    free(cur_slot->msg);
    cur_slot->msg = NULL;
    
    // Shift all slots to the left
    for (int i = 0; i < RECEIVE_WINDOW_SLOT_SIZE - 1; i++) {
      sock->receive_window[i] = sock->receive_window[i + 1];
    }
    sock->receive_window[RECEIVE_WINDOW_SLOT_SIZE - 1].is_used = 0;
    sock->receive_window[RECEIVE_WINDOW_SLOT_SIZE - 1].msg = NULL;
  }
}

void transmit_send_window(foggy_socket_t *sock) {
  if (sock->send_window.empty()) return;

  struct timespec current_time;
  clock_gettime(CLOCK_MONOTONIC, &current_time);
  
  uint32_t effective_window = MIN(sock->window.congestion_window, 
                                   sock->window.advertised_window);
  
  // Calculate bytes in flight (sent but not acknowledged)
  uint32_t bytes_in_flight = 0;
  for (auto it = sock->send_window.begin(); it != sock->send_window.end(); ++it) {
    if (it->is_sent) {
      foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)it->msg;
      bytes_in_flight += get_payload_len(it->msg);
    }
  }
  
  // Try to send packets that fit within the window
  for (auto it = sock->send_window.begin(); it != sock->send_window.end(); ++it) {
    foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)it->msg;
    uint16_t payload_len = get_payload_len(it->msg);
    
    if (!it->is_sent) {
      // New packet - check if we can send it
      if (bytes_in_flight + payload_len <= effective_window) {
        
        it->is_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &it->send_time);
        it->timeout_interval = TIMEOUT_MS;
        it->retransmissions = 0;
        
        sendto(sock->socket, it->msg, get_plen(hdr), 0,
               (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
        
        bytes_in_flight += payload_len;
      } else {
        // Window is full, stop sending
        break;
      }
    } else {
      // Check for timeout
      long elapsed_ms = (current_time.tv_sec - it->send_time.tv_sec) * 1000 +
                        (current_time.tv_nsec - it->send_time.tv_nsec) / 1000000;
      
      if (elapsed_ms >= it->timeout_interval) {
        // Timeout occurred - retransmit and adjust congestion control
        
        pthread_mutex_lock(&(sock->window.ack_lock));
        sock->window.ssthresh = MAX(sock->window.congestion_window / 2, MSS);
        sock->window.congestion_window = MSS;
        sock->window.reno_state = RENO_SLOW_START;
        sock->window.dup_ack_count = 0;
        pthread_mutex_unlock(&(sock->window.ack_lock));
        
        
        clock_gettime(CLOCK_MONOTONIC, &it->send_time);
        it->timeout_interval = TIMEOUT_MS;
        it->retransmissions++;
        
        sendto(sock->socket, it->msg, get_plen(hdr), 0,
               (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
      }
    }
  }
}

void receive_send_window(foggy_socket_t *sock) {
  // Pop out the packets that have been ACKed
  while (!sock->send_window.empty()) {
    send_window_slot_t& slot = sock->send_window.front();
    foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)slot.msg;
    
    uint32_t seq = get_seq(hdr);
    uint16_t payload_len = get_payload_len(slot.msg);
    uint32_t end_seq = seq + payload_len;
    
    // Check if this packet has been fully acknowledged
    pthread_mutex_lock(&(sock->window.ack_lock));
    uint32_t last_ack = sock->window.last_ack_received;
    pthread_mutex_unlock(&(sock->window.ack_lock));
    
    if (!after(last_ack, seq)) {
      // This packet hasn't been acknowledged yet
      break;
    }
    
    // Packet has been acknowledged, remove it
    sock->send_window.pop_front();
    free(slot.msg);
  }
}
