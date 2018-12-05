#define P4WNP1_GENL_NAME "p4wnp1" // Name of P4wnP1 Generic Netlink Family
#define P4WNP1_MULTICAST_GROUP_NAME_DWC2 "dwc2"

// attributes for payload (first dummy attribute is needed)
enum p4wnp1_attrs {
	P4WNP1_ATTR_DUMMY, // dummy/unspec attribute needed at first position (maybe attr IDs have to start at 1)
	P4WNP1_ATTR_CONNECTION_STATE,

	__P4WNP1__ATTR_AFTER_LAST,
	NUM_P4WNP1_ATTR = __P4WNP1__ATTR_AFTER_LAST,
	P4WNP1_ATTR_MAX = __P4WNP1__ATTR_AFTER_LAST - 1
};

// Commands available in the genl family 
enum p4wnp1_commands {
	P4WNP1_CMD_CONNECTION_STATE,

	__P4WNP1_CMD_AFTER_LAST,
	P4WNP1_CMD_MAX = __P4WNP1_CMD_AFTER_LAST - 1
};


int netlink_init(void);
void netlink_release(void);
void update_con_state(uint32_t new_state);
void send_conn_state_genl_mcast(u8 *data); //send connection state as genl mcast
void tasklet_fn_mcast_connection_state(unsigned long); //tasklet function: calls send_conn_state_genl_mcast; argument: 0 for disconnect, 1 for connect



