#pragma once
#include <stdint.h>
typedef void (*hci_event_cb)(uint8_t code, const uint8_t *p, int len);
typedef void (*hci_acl_cb)(const uint8_t *pkt, int len);
extern int hci_verbose;
extern volatile int hci_dead;
int hci_open(uint16_t vid, uint16_t pid);
void hci_close(void);
void hci_set_callbacks(hci_event_cb e, hci_acl_cb a);
int hci_send_cmd(uint16_t op, const void *params, uint8_t plen);
int hci_cmd(uint16_t op, const void *params, uint8_t plen, uint8_t *rsp, int *rlen, double timeout);
int hci_send_acl(uint16_t handle, const void *data, uint16_t len);
void hci_run(double seconds);
int hci_read_version(uint8_t *hci_ver, uint16_t *hci_rev, uint8_t *lmp_ver, uint16_t *lmp_sub);
int rtl_load_firmware(const char *fw_path, const char *cfg_path);
