int netlink_init(void);
void netlink_release(void);
void nl_mcast_send_data(u8 *data, u32 len);
void update_con_state(uint32_t new_state);