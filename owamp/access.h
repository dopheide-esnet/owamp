/*
 * $Id$
 */
#ifndef	_OWP_ACCESS_H_
#define	_OWP_ACCESS_H_

#include <I2util/util.h>
#include <owamp/owamp.h>

#define OWPMAX_LINE 1024

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#define OWP_KID_LEN 16
#define OWP_PASSWD_LEN 16
#define OWP_HEX_PASSWD_LEN (OWP_PASSWD_LEN * 2)
#define OWP_MAX_CLASS_LEN 32

typedef u_int64_t owp_lim_t;

/*
** This structure is used to keep track of usage resources.
*/
typedef struct owamp_limits {
	owp_lim_t values[6];
} owp_lim;

/*
** Inidices to refer to particular limits/attributes.
*/
#define OWP_LIM_BANDWIDTH 0      /* bandwidth - octets/sec */
#define OWP_LIM_SPACE     1 	 /* disk space - octets    */
#define OWP_LIM_EXPIRY    2      /* expiry offset - sec    */
#define OWP_LIM_DOC       3      /* delete-on-close flag   */
#define OWP_LIM_DOF       4      /* delete-on-fetch flag   */
#define OWP_LIM_OPEN_OK   5      /* open_mode-ok flag      */

typedef struct owp_tree_node *owp_tree_node_ptr;

typedef struct owp_tree_node {
	char* data;
	owp_tree_node_ptr next_sibling;
	owp_tree_node_ptr first_child;
	owp_tree_node_ptr parent;
	owp_lim           limits;
} owp_tree_node;

/*
** Generic type for a node-processing function.
*/
typedef void (*owp_node_func)(
	owp_tree_node_ptr node,       /* Node to be processed.         */
	void *data                    /* Additional data to be passed. */
);

typedef struct {
	I2table ip2class;
	I2table class2node;
	I2table passwd;
	owp_tree_node_ptr root; /* Root class of the tree hierarchy. */
} owp_policy_data;

typedef struct owp_access_id {
	u_int32_t addr4;
	u_int8_t  addr6[16];
	u_int8_t  offset;                       /* not meaningful for KID */
	char      kid[OWP_KID_LEN + 1];
	int    type;  /* OWP_IDTYPE_KID, OWP_IDTYPE_IPv4, OWP_IDTYPE_IPv6 */
} owp_access_id;

typedef struct owp_access_netmask {
	u_int32_t addr4;            /* In host byte order. */
	u_int8_t  addr6[16];        /* In network byte order. */
	u_int8_t  offset;
	int       af;               /* AF_INET, AF_INET6 */
} owp_access_netmask;

typedef struct owp_kid_data {
	char passwd[OWP_HEX_PASSWD_LEN + 1];
	char class[OWP_MAX_CLASS_LEN + 1];
} owp_kid_data;

#define OWP_OK            0
#define OWP_ERR           1
#define OWP_EOF           2
#define OWP_LAST          3
#define OWP_END_DESCR     4

/*
** Fetch node corresponfing to a given classname string.
*/
extern owp_tree_node_ptr owp_class2node(char *class, I2table hash);

/*
** Generic tree traversal function. Caller provides the node-processing
** function <func> along with any necessary data.
*/
extern void
owp_tree_iterate_df(owp_tree_node_ptr root, owp_node_func func, void *data);

/*
** Default function for printing node info to a given file stream
** (to be cast as FILE* from <data>)
*/
extern void owp_print_node(owp_tree_node_ptr node, void *data);

extern char *ipaddr2class(u_int32_t ip, I2table ip2class_hash);
extern char *owp_kid2passwd(const char *kid, int len, owp_policy_data* policy);
extern char *owp_kid2class(const char *kid, int len, owp_policy_data* policy);

extern char *owp_sockaddr2class(struct sockaddr *addr, owp_policy_data* policy);
extern void owp_print_ip2class_binding(const struct I2binding *p, FILE* fp);
extern void owp_print_strnet(owp_access_netmask *ptr, FILE* fp);
extern void owp_print_netmask(const I2datum *key, FILE* fp);
extern void owp_print_kid2data_binding(const struct I2binding *p, FILE* fp);
extern void print_class2limits_binding(const struct I2binding *p, FILE* fp);
extern void owp_print_class2node_binding(const struct I2binding *p, FILE* fp);
extern char *owamp_denumberize(unsigned long addr);
extern OWPBoolean owp_get_aes_key(void *app_data, const char *kid, \
	u_int8_t *key_ret, OWPErrSeverity	*err_ret);

extern OWPBoolean
owp_check_control(
	OWPControl	cntrl,
	void		*app_data,
	OWPSessionMode	mode,
	const char	*kid,
	struct sockaddr	*local_sa_addr,
	struct sockaddr	*remote_sa_addr,
	OWPErrSeverity	*err_ret
	);

extern OWPBoolean owp_check_test(void *app_data, OWPSessionMode	mode,
	const char *kid, OWPBoolean local_sender,
	struct sockaddr *local_sa_addr, struct sockaddr	*remote_sa_addr,
	OWPTestSpec	*test_spec, OWPErrSeverity *err_ret);
extern unsigned long OWAMPGetBandwidth(owp_lim* lim);
extern unsigned long OWAMPGetSpace(owp_lim* lim);
extern unsigned long OWAMPGetNumSessions(owp_lim* lim);
extern I2datum* owp_raw2datum(const void *bytes, size_t len);
extern void owp_datum_free(I2datum *datum);
extern int owp_read_class2limits2(
	OWPContext ctx,
	const char *class2limits,
	owp_policy_data* policy
	);

extern owp_policy_data
*OWPPolicyInit(
	OWPContext	ctx,
	char		*ip2class_file,
	char		*class2limits_file,
	char		*passwd_file,
	OWPErrSeverity	*err_ret
);

/* chunk_buf functions and data structures */
typedef struct owp_chunk_buf {
	char *data;             /* data */
	char *cur;              /* current location to be written */
	size_t alloc_size;      /* amount of memory allocated for data,
				   ignoring the sentinel */
	char *sentinel;         /* this location keeps the final '\0' and
				   cannot be overwritten */
} owp_chunk_buf, *owp_chunk_buf_ptr;

extern int owp_buf_init(owp_chunk_buf_ptr bufptr, size_t len);
void owp_buf_reset(owp_chunk_buf_ptr bufptr);
extern void owp_buf_free(owp_chunk_buf_ptr buf);
extern int owp_symbol_save(OWPContext ctx, owp_chunk_buf_ptr buf, int c);
extern void owp_buf_print(FILE *fp, owp_chunk_buf_ptr buf);

#endif	/*	_OWP_ACCESS_H_	*/