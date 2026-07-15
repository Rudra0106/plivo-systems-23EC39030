/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdbool.h>
#include <poll.h>

#define PAYLOAD_SIZE 160
#define MAX_FRAMES 100000

uint8_t jitter_buffer[MAX_FRAMES][PAYLOAD_SIZE];
bool has_frame[MAX_FRAMES];

uint8_t fec_buffer[MAX_FRAMES][PAYLOAD_SIZE];
bool has_fec[MAX_FRAMES];

double get_current_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + (tv.tv_usec / 1000000.0);
}
void try_recover(uint32_t base_seq) {
    if (!has_fec[base_seq]) return;
    
    bool has_even = has_frame[base_seq];
    bool has_odd = has_frame[base_seq + 1];
    
    if (has_even && !has_odd) {
        for(int i=0; i<PAYLOAD_SIZE; i++) {
            jitter_buffer[base_seq + 1][i] = fec_buffer[base_seq][i] ^ jitter_buffer[base_seq][i];
        }
        has_frame[base_seq + 1] = true;
    } else if (!has_even && has_odd) {
        for(int i=0; i<PAYLOAD_SIZE; i++) {
            jitter_buffer[base_seq][i] = fec_buffer[base_seq][i] ^ jitter_buffer[base_seq + 1][i];
        }
        has_frame[base_seq] = true;
    }
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) return 1;

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    const char *t0_str = getenv("T0");
    const char *delay_str = getenv("DELAY_MS");
    if (!t0_str || !delay_str) return 1;
    double t0 = atof(t0_str);
    double delay_s = atof(delay_str) / 1000.0;

    memset(has_frame, 0, sizeof(has_frame));
    memset(has_fec, 0, sizeof(has_fec));
    uint32_t next_playout_seq = 0;

    struct pollfd pfd;
    pfd.fd = in_fd;
    pfd.events = POLLIN;

    for (;;) {
        double now = get_current_time();
        double deadline = t0 + delay_s + (next_playout_seq * 0.020);

        if (has_frame[next_playout_seq]) {
            uint8_t out_buf[4 + PAYLOAD_SIZE];
            uint32_t net_seq = htonl(next_playout_seq);
            memcpy(out_buf, &net_seq, 4);
            memcpy(out_buf + 4, jitter_buffer[next_playout_seq], PAYLOAD_SIZE);
            
            sendto(out_fd, out_buf, sizeof(out_buf), 0, (struct sockaddr *)&player, sizeof(player));
            next_playout_seq++;
            continue; 
        } else if (now >= deadline) {
            next_playout_seq++;
            continue;
        }

        double wait_time_s = deadline - now;
        int timeout_ms = (int)(wait_time_s * 1000.0);
        if (timeout_ms < 0) timeout_ms = 0;

        int ret = poll(&pfd, 1, timeout_ms);
        if (ret > 0) {
            unsigned char buf[2048];
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            
            if (n == 4 + PAYLOAD_SIZE) {
                uint32_t raw_seq;
                memcpy(&raw_seq, buf, 4);
                uint32_t host_seq = ntohl(raw_seq);
                
                bool is_fec = (host_seq & 0x80000000) != 0;
                uint32_t actual_seq = host_seq & 0x7FFFFFFF;

                if (actual_seq < MAX_FRAMES) {
                    if (is_fec) {
                        if (!has_fec[actual_seq]) {
                            memcpy(fec_buffer[actual_seq], buf + 4, PAYLOAD_SIZE);
                            has_fec[actual_seq] = true;
                            try_recover(actual_seq);
                        }
                    } else {
                        if (!has_frame[actual_seq]) {
                            memcpy(jitter_buffer[actual_seq], buf + 4, PAYLOAD_SIZE);
                            has_frame[actual_seq] = true;
                            uint32_t base_seq = (actual_seq % 2 == 0) ? actual_seq : actual_seq - 1;
                            try_recover(base_seq);
                        }
                    }
                }
            }
        }
    }
    return 0;
}