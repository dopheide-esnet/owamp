/*
 * ex: set tabstop=4 ai expandtab softtabstop=4 shiftwidth=4:
 * -*- mode: c-basic-indent: 4; tab-width: 4; indent-tabls-mode: nil -*-
 *      $Id$
 */
/************************************************************************
 *                                                                       *
 *                           Copyright (C)  2002                         *
 *                               Internet2                               *
 *                           All Rights Reserved                         *
 *                                                                       *
 ************************************************************************/
/*
 *    File:         owping.c
 *
 *    Author:       Jeff Boote
 *                  Anatoly Karp
 *                  Internet2
 *
 *    Date:         Thu Apr 25 12:22:31  2002
 *
 *    Description:    
 *
 *    Initial implementation of owping commandline application. This
 *    application will measure active one-way udp latencies.
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <I2util/util.h>
#include <owamp/owamp.h>

#include "./owpingP.h"

/*
 * The owping context
 */
static  ow_ping_trec    ping_ctx;
static  I2ErrHandle     eh;
static  char            tmpdir[PATH_MAX+1];
static  u_int8_t        aesbuff[16];

    static void
print_conn_args()
{
    fprintf(stderr, "%s\n\n%s\n%s\n%s\n%s\n",
            "              [Connection Args]",
            "   -A authmode    requested modes: [A]uthenticated, [E]ncrypted, [O]pen",
            "   -k keyfile     AES keyfile to use with Authenticated/Encrypted modes",
            "   -u username    username to use with Authenticated/Encrypted modes",
            "   -S srcaddr     use this as a local address for control connection and tests");
}

    static void
print_test_args()
{
    fprintf(stderr, "%s\n\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n",
            "              [Test Args]",
            "   -f | -F file   perform one-way test from testhost [and save results to file]",
            "   -t | -T file   perform one-way test to testhost [and save results to file]",
            "   -c count       number of test packets",
            "   -i wait        mean average time between packets (seconds)",
            "   -L timeout     maximum time to wait for a packet before declaring it lost (seconds)",
            "   -s padding     size of the padding added to each packet (bytes)",
            "   -z delayStart  time to wait before executing test (seconds)"
            );
}

    static void
print_output_args()
{
    fprintf(stderr, "%s\n\n%s\n%s\n%s\n%s\n%s\n",
            "              [Output Args]",
            "   -h             print this message and exit",
            "   -Q             run the test and exit without reporting statistics",
            "   -R             print RAW data: \"SEQNO STIME SS SERR RTIME RS RERR TTL\\n\"",
            "   -v             print out individual delays",
            "   -a alpha       report an additional percentile level for the delays"
           );
}

    static void
usage(const char *progname, const char *msg)
{
    if(msg) fprintf(stderr, "%s: %s\n", progname, msg);
    if (!strcmp(progname, "owping")) {
        fprintf(stderr,
                "usage: %s %s\n%s\n", 
                progname, "[arguments] testaddr [servaddr]",
                "[arguments] are as follows: "
               );
        fprintf(stderr, "\n");
        print_conn_args();

        fprintf(stderr, "\n");
        print_test_args();

        fprintf(stderr, "\n");
        print_output_args();

    } else if (!strcmp(progname, "owstats")) {
        fprintf(stderr,
                "usage: %s %s\n%s\n",
                progname, "[arguments] sessionfile",
                "[arguments] are as follows: "
               );
        fprintf(stderr, "\n");
        print_output_args();
    } else if (!strcmp(progname, "owfetch")) {
        fprintf(stderr,
                "usage: %s %s\n%s\n",
                progname, "[arguments] servaddr [SID savefile]+",
                "[arguments] are as follows: "
               );
        fprintf(stderr, "\n");
        print_conn_args();
        fprintf(stderr, "\n");
        print_output_args();
    }
    else{
        fprintf(stderr,
                "usage: %s is not a known name for this program.\n",progname);
    }

    fprintf(stderr, "Distribution: %s\n", PACKAGE_STRING);

    return;
}

static void
    FailSession(
            OWPControl    control_handle    __attribute__((unused))
            )
{
    /*
     * Session denied - report error, close connection, and exit.
     */
    I2ErrLog(eh, "Session Failed!");
    fflush(stderr);

    /* TODO: determine "reason" for denial and report */
    (void)OWPControlClose(ping_ctx.cntrl);
    exit(1);
}

#define THOUSAND 1000.0

/* Width of Fetch receiver window. */
#define OWP_WIN_WIDTH   64

#define OWP_MAX_N           100  /* N-reordering statistics parameter */

/*
 ** Generic state to be maintained by client during Fetch.
 */
typedef struct fetch_state {
    FILE*       fp;        /* stream to report records    */
    char        sid_name[sizeof(OWPSID)*2+1];/* hex encoded sid    */
    OWPDataRec  window[OWP_WIN_WIDTH]; /* window of read records*/
    OWPDataRec  last_out;    /* last processed record    */
    int         cur_win_size;    /* number of records in the window*/
    double      tmin;        /* min delay            */
    double      tmax;        /* max delay            */
    u_int32_t   num_received;    /* number of good received packets*/
    u_int32_t   dup_packets;    /* number of duplicate packets    */
    int         order_disrupted;/* flag                */
    u_int32_t   max_seqno;    /* max sequence number seen    */
    u_int32_t   *buckets;    /* array of buckets of counts    */
    char        *from;        /* Endpoints in printable format*/
    char        *to;
    char        *fromserv;
    char        *toserv;
    u_int32_t   count_out;    /* number of printed packets    */

    /*
     * Worst error for all packets in test.
     */
    double      errest;
    int         sync;        /* flag set no unsync packets    */

    /* N-reodering state variables. */
    u_int32_t   m[OWP_MAX_N];    /* We have m[j-1] == number of
                                     j-reordered packets.    */
    u_int32_t   ring[OWP_MAX_N];/* Last sequence numbers seen.    */
    u_int32_t   r;        /* Ring pointer for next write.    */
    u_int32_t   l;        /* Number of seq numbers read.    */

    u_int32_t   ttl_count[256];

} fetch_state;

#define OWP_CMP(a,b) ((a) < (b))? -1 : (((a) == (b))? 0 : 1)

/*
 ** The function returns -1. 0 or 1 if the first record's sequence
 ** number is respectively less than, equal to, or greater than that 
 ** of the second.
 */
int
owp_seqno_cmp(
        OWPDataRec    *a,
        OWPDataRec    *b
        )
{
    assert(a); assert(b);
    return OWP_CMP(a->seq_no, b->seq_no);
}

/*
 ** Find the right spot in the window to insert the new record <rec>
 ** Return max {i| 0 <= i <= cur_win_size-1 and <rec> is later than the i_th
 ** record in the state window}, or -1 if no such index is found.
 */
int
look_for_spot(
        fetch_state    *state,
        OWPDataRec    *rec
        )
{
    int i;
    assert(state->cur_win_size);

    for (i = state->cur_win_size - 1; i >= 0; i--) {
        if (owp_seqno_cmp(&state->window[i], rec) < 0)
            return i;
    }

    return -1;
}


/*
 ** Generic function to output timestamp record <rec> in given format
 ** as encoded in <state>.
 */
void
owp_record_out(
        fetch_state    *state,
        OWPDataRec    *rec
        )
{
    double delay;

    assert(rec);
    assert(state);

    if (!ping_ctx.opt.records)
        return;

    assert(state->fp);

    if(!(state->count_out++ % 21)){
        fprintf(state->fp,
                "--- owping test session from [%s]:%s to [%s]:%s ---\n",
                state->from,state->fromserv,
                state->to,state->toserv);
        fprintf(state->fp,"SID: %s\n",state->sid_name);
    }

    delay = OWPDelay(&rec->send, &rec->recv);

    if (!OWPIsLostRecord(rec)) {
        if (rec->send.sync && rec->recv.sync) {
            double prec = OWPGetTimeStampError(&rec->send) +
                OWPGetTimeStampError(&rec->recv);
            fprintf(state->fp, 
                    "seq_no=%-10u delay=%.3f ms       (sync, precision %.3f ms)\n", 
                    rec->seq_no, delay*THOUSAND, 
                    prec*THOUSAND);
        } else
            fprintf(state->fp, 
                    "seq_no=%u delay=%.3f ms (unsync)\n",
                    rec->seq_no, delay*THOUSAND);
        return;
    }

    fprintf(state->fp, "seq_no=%-10u *LOST*\n", rec->seq_no);

    return;
}

#define OWP_MAX_BUCKET  (OWP_NUM_LOW + OWP_NUM_MID + OWP_NUM_HIGH - 1)

#define OWP_NUM_LOW         50000
#define OWP_NUM_MID         100000
#define OWP_NUM_HIGH        49900

#define OWP_CUTOFF_A        (double)(-50.0)
#define OWP_CUTOFF_B        (double)0.0
#define OWP_CUTOFF_C        (double)0.1
#define OWP_CUTOFF_D        (double)50.0

const double mesh_low = (OWP_CUTOFF_B - OWP_CUTOFF_A)/OWP_NUM_LOW;
const double mesh_mid = (OWP_CUTOFF_C - OWP_CUTOFF_B)/OWP_NUM_MID;
const double mesh_high = (OWP_CUTOFF_D - OWP_CUTOFF_C)/OWP_NUM_HIGH;

    int
owp_bucket(double delay)
{
    if (delay < OWP_CUTOFF_A)
        return 0;

    if (delay < OWP_CUTOFF_B)
        return OWP_NUM_LOW + (int)(delay/mesh_low);

    if (delay < OWP_CUTOFF_C)
        return OWP_NUM_LOW +  (int)(delay/mesh_mid);

    if (delay < OWP_CUTOFF_D)
        return OWP_NUM_LOW + OWP_NUM_MID 
            + (int)((delay - OWP_CUTOFF_C)/mesh_high);

    return OWP_MAX_BUCKET;
}

void
owp_update_stats(
        fetch_state    *state,
        OWPDataRec    *rec
        )
{
    double delay;  
    double errest;
    int bucket;

    assert(state); assert(rec);

    if (state->num_received && !owp_seqno_cmp(rec, &state->last_out)){
        state->dup_packets++;
        state->num_received++;
        return;
    }

    if (rec->seq_no > state->max_seqno)
        state->max_seqno = rec->seq_no;
    if (OWPIsLostRecord(rec))
        return;
    state->num_received++;

    delay =  OWPDelay(&rec->send, &rec->recv);

    errest = OWPGetTimeStampError(&rec->send);
    errest += OWPGetTimeStampError(&rec->recv);

    if(errest > state->errest){
        state->errest = errest;
    }

    if (!rec->send.sync || !rec->recv.sync)
        state->sync = 0;

    bucket = owp_bucket(delay);

    assert((0 <= bucket) && (bucket <= OWP_MAX_BUCKET));
    state->buckets[bucket]++;

    if (delay < state->tmin)
        state->tmin = delay;
    if (delay > state->tmax)
        state->tmax = delay;


    memcpy(&state->last_out, rec, sizeof(*rec));
}

/*
 ** Given a number <alpha> in [0, 1], compute
 ** min {x: F(x) >= alpha}
 ** where F is the empirical distribution function (in our case,
 ** with a fuzz factor due to use of buckets.
 */
    double
owp_get_percentile(fetch_state *state, double alpha)
{
    int i;
    double sum = 0;
    u_int32_t unique = state->num_received - state->dup_packets;

    assert((0.0 <= alpha) && (alpha <= 1.0));

    for (i = 0; (i <= OWP_MAX_BUCKET) && (sum < alpha*unique); i++)
        sum += state->buckets[i];

    if (i <= OWP_NUM_LOW)
        return OWP_CUTOFF_A + i*mesh_low;
    if (i <= OWP_NUM_LOW + OWP_NUM_MID)
        return OWP_CUTOFF_B + (i - OWP_NUM_LOW)*mesh_mid;
    return OWP_CUTOFF_C + (i - (OWP_NUM_LOW+OWP_NUM_MID))*mesh_high;

    return 0.0;
}

/*
 ** Processs a single record, updating statistics and internal state.
 ** Return 0 on success, or -1 on failure, 1 to stop parsing data.
 */
#define OWP_LOOP(x)         ((x) >= 0? (x): (x) + OWP_MAX_N)

static int
do_single_record(
        OWPDataRec    *rec,
        void        *calldata
        ) 
{
    int i;
    fetch_state *state = (fetch_state*)calldata;
    u_int32_t j;

    assert(state);

    owp_record_out(state, rec); /* Output is done in all cases. */

    if(OWPIsLostRecord(rec)) {
        owp_update_stats(state, rec);
        return 0;       /* May do something better later. */
    }

    /* If ordering is important - handle it here. */
    if(state->order_disrupted)
        return 0;

    /*
     * Count ttl values here
     */
    state->ttl_count[rec->ttl]++;

    /* Update N-reordering state. */
    for(j = 0; j < MIN(state->l, OWP_MAX_N); j++) { 
        if(rec->seq_no 
                >= state->ring[OWP_LOOP((int)(state->r - j - 1))])
            break;
        state->m[j]++;
    }
    state->ring[state->r] = rec->seq_no;
    state->l++;
    state->r = (state->r + 1) % OWP_MAX_N;

    if(state->cur_win_size < OWP_WIN_WIDTH){/* insert - no stats updates*/
        if(state->cur_win_size) { /* Grow window. */
            int num_records_to_move;
            i = look_for_spot(state, rec);
            num_records_to_move = state->cur_win_size - i - 1;

            /* Cut and paste if needed - then insert. */
            if(num_records_to_move) 
                memmove(&state->window[i+2], 
                        &state->window[i+1], 
                        num_records_to_move*sizeof(*rec));
            memcpy(&state->window[i+1], rec, sizeof(*rec)); 
        }
        else{
            /* Initialize window. */
            memmove(&state->window[0], rec, sizeof(*rec));
        }
        state->cur_win_size++;
    }
    else{
        /* rotate - update state*/
        OWPDataRec    *out_rec = rec;        
        if(state->num_received &&
                (rec->seq_no < state->last_out.seq_no)) {
            state->order_disrupted = 1;
            /* terminate parsing */
            return 1; 
        }

        i = look_for_spot(state, rec);

        if (i != -1)
            out_rec = &state->window[0];
        owp_update_stats(state, out_rec);

        /* Update the window.*/
        if (i != -1) {  /* Shift if needed - then insert.*/
            if (i) 
                memmove(&state->window[0],
                        &state->window[1], i*sizeof(*rec));
            memcpy(&state->window[i], rec, sizeof(*rec));
        } 
    }

    return 0;
}

/*
 ** Print out summary results, ping-like style. sent + dup == lost +recv.
 */
int
owp_do_summary(
        fetch_state *state
        )
{
    u_int8_t    minttl=255;
    u_int8_t    maxttl=0;
    u_int8_t    nttl=0;
    double      min = ((double)(state->tmin)) * THOUSAND;    /* msec */
    u_int32_t   sent = state->max_seqno + 1;
    u_int32_t   lost = state->dup_packets + sent - state->num_received; 
    double      percent_lost = (100.0*(double)lost)/(double)sent;
    int         j;

    assert(state); assert(state->fp);

    fprintf(state->fp,
            "\n--- owping statistics from [%s]:%s to [%s]:%s ---\n",
            state->from,state->fromserv,state->to,state->toserv);
    fprintf(state->fp,"SID: %s\n",state->sid_name);
    if (state->dup_packets)
        fprintf(state->fp, 
                "%u packets transmitted, %u packets lost (%.1f%% loss), %u duplicates\n",
                sent, lost, percent_lost, state->dup_packets);
    else    
        fprintf(state->fp, 
                "%u packets transmitted, %u packets lost (%.1f%% loss)\n",
                sent ,lost, percent_lost);
    if (!state->num_received)
        goto done;

    if (state->sync)
        fprintf(state->fp, 
                "one-way delay min/median = %.3f/%.3f ms  (precision %.5g s)\n",
                min, owp_get_percentile(state, 0.5)*THOUSAND,
                state->errest);
    else
        fprintf(state->fp, 
                "one-way delay min/median = %.3f/%.3f ms  (unsync)\n", 
                min, owp_get_percentile(state, 0.5)*THOUSAND);

    /*
     * Report ttl's.
     */
    for(j=0;j<255;j++){
        if(!state->ttl_count[j])
            continue;
        nttl++;
        if(j<minttl)
            minttl = j;
        if(j>maxttl)
            maxttl = j;
    }

    if(nttl < 1){
        fprintf(state->fp,"TTL not reported\n");
    }
    else if(nttl == 1){
        fprintf(state->fp,"TTL is %d (consistently)\n",minttl);
    }
    else{
        fprintf(state->fp,"TTL takes %d values; min=%d, max=%d\n",
                nttl,minttl,maxttl);
    }



    for (j = 0; j < OWP_MAX_N && state->m[j]; j++)
        fprintf(state->fp,
                "%d-reordering = %f%%\n", j+1, 
                100.0*state->m[j]/(state->l - j - 1));
    if (j == 0) 
        fprintf(state->fp, "no reordering\n");
    else 
        if (j < OWP_MAX_N) 
            fprintf(state->fp, "no %d-reordering\n", j + 1);
        else 
            fprintf(state->fp, 
                    "only up to %d-reordering is handled\n", OWP_MAX_N);

    if ((ping_ctx.opt.percentile - 50.0) > 0.000001
            || (ping_ctx.opt.percentile - 50.0) < -0.000001) {
        float x = ping_ctx.opt.percentile/100.0;
        fprintf(state->fp, 
                "%.2f percentile of one-way delays: %.3f ms\n",
                ping_ctx.opt.percentile,
                owp_get_percentile(state, x) * THOUSAND);
    }
done:    
    fprintf(state->fp, "\n");

    return 0;
}

/*
 * RAW ascii format is:
 * "SEQ STIME SS SERR RTIME RS RERR\n"
 * name     desc                type
 * SEQ      sequence number     unsigned long
 * STIME    sendtime            owptimestamp (%020llu)
 * SS       send synchronized   boolean unsigned
 * SERR     send err estimate   float (%g)
 * RTIME    recvtime            owptimestamp (%020llu)
 * RS       recv synchronized   boolean unsigned
 * RERR     recv err estimate   float (%g)
 * TTL      ttl                 unsigned short
 */
#define RAWFMT "%lu %020llu %u %g %020llu %u %g %u\n"
static int
printraw(
        OWPDataRec    *rec,
        void        *udata
        )
{
    FILE        *out = (FILE*)udata;

    fprintf(out,RAWFMT,(unsigned long)rec->seq_no,
            rec->send.owptime,rec->send.sync,
            OWPGetTimeStampError(&rec->send),
            rec->recv.owptime,rec->recv.sync,
            OWPGetTimeStampError(&rec->recv),
            rec->ttl);
    return 0;
}

/*
 ** Master output function - reads the records from the disk
 ** and prints them to <out> in a style specified by <type>.
 ** Its value is interpreted as follows:
 ** 0 - print out send and recv timestamsps for each record in machine-readable
 ** format;
 ** 1 - print one-way delay in msec for each record, and final summary
 **     (original ping style: max/avg/min/stdev) at the end.
 */
int
do_records_all(
        OWPContext    ctx,
        FILE        *output,
        FILE        *fp,
        char        *from,
        char        *to
        )
{
    int            i, num_buckets;
    u_int32_t        num_rec;
    OWPSessionHeaderRec    hdr;
    fetch_state        state;
    char            frombuf[NI_MAXHOST+1];
    char            tobuf[NI_MAXHOST+1];
    char            fromserv[NI_MAXSERV+1];
    char            toserv[NI_MAXSERV+1];

    if(!(num_rec = OWPReadDataHeader(ctx,fp,&hdr))){
        I2ErrLog(eh, "OWPReadDataHeader:Empty file?");
        return -1;
    }

    if(ping_ctx.opt.raw){
        if(OWPParseRecords(ctx,fp,num_rec,hdr.version,printraw,output)
                < OWPErrWARNING){
            I2ErrLog(eh,"OWPParseRecords(): %M");
            return -1;
        }
        return 0;
    }

    memset(&state,0,sizeof(state));

    /*
     * Set sid
     */
    if(hdr.header){
        I2HexEncode(state.sid_name,hdr.sid,sizeof(OWPSID));
    }

    /*
     * Get pretty names...
     */
    if(!hdr.header || getnameinfo((struct sockaddr*)&hdr.addr_sender,
                hdr.addr_len,
                frombuf,sizeof(frombuf),
                fromserv,sizeof(fromserv),
                NI_NUMERICSERV)){
        if(from){
            strncpy(frombuf,from,NI_MAXHOST);
        }else{
            strcpy(frombuf,"***");
        }
        fromserv[0] = '\0';
    }
    state.from = frombuf;
    state.fromserv = fromserv;

    if(!hdr.header || getnameinfo((struct sockaddr*)&hdr.addr_receiver,
                hdr.addr_len,
                tobuf,sizeof(tobuf),
                toserv,sizeof(toserv),
                NI_NUMERICSERV)){
        if(from){
            strncpy(tobuf,to,NI_MAXHOST);
        }else{
            strcpy(tobuf,"***");
        }
        toserv[0] = '\0';
    }
    state.to = tobuf;
    state.toserv = toserv;

    /*
     * Initialize fields of state to keep track of.
     */
    state.fp = output;
    state.cur_win_size = 0;
    state.tmin = 9999.999;
    state.tmax = 0.0;
    state.num_received = state.dup_packets = state.max_seqno = 0;

    state.order_disrupted = 0;

    state.count_out = 0;

    state.errest = 0.0;
    state.sync = 1;

    /* N-reodering fields/ */
    state.r = state.l = 0;
    for (i = 0; i < OWP_MAX_N; i++) 
        state.m[i] = 0;

    num_buckets = OWP_NUM_LOW + OWP_NUM_MID + OWP_NUM_HIGH;

    state.buckets 
        = (u_int32_t *)malloc(num_buckets*sizeof(*(state.buckets)));
    if (!state.buckets) {
        I2ErrLog(eh, "FATAL: main: malloc(%d) failed: %M",num_buckets);
        exit(1);
    }
    for (i = 0; i <= OWP_MAX_BUCKET; i++)
        state.buckets[i] = 0;


    if(OWPParseRecords(ctx,fp,num_rec,hdr.version,do_single_record,&state)
            < OWPErrWARNING){
        I2ErrLog(eh,"OWPParseRecords():%M");
        return -1;
    }

    /* Stats are requested and failed to keep records sorted - redo */
    if (state.order_disrupted) {
        I2ErrLog(eh, "Severe out-of-order condition observed.");
        I2ErrLog(eh, 
                "Producing statistics for this case is currently unimplemented.");
        return 0;
    }

    /* Incorporate remaining records left in the window. */
    for (i = 0; i < state.cur_win_size; i++)
        owp_update_stats(&state, &state.window[i]);

    owp_do_summary(&state);
    free(state.buckets);
    return 0;
}

static FILE *
tfile(
        OWPContext    eh
     )
{
    char    fname[PATH_MAX+1];
    int    fd;
    FILE    *fp;

    strcpy(fname,tmpdir);
    strcat(fname,_OWPING_PATH_SEPARATOR);
    strcat(fname,_OWPING_TMPFILEFMT);

    if((fd = mkstemp(fname)) < 0){
        I2ErrLog(eh,"mkstemp(%s): %M",fname);
        return NULL;
    }

    if( !(fp = fdopen(fd,"w+b"))){
        I2ErrLog(eh,"fdopen(%s:(%d)): %M",fname,fd);
        return NULL;
    }

    if(unlink(fname) != 0){
        I2ErrLog(eh,"unlink(%s): %M",fname);
        while((fclose(fp) != 0) && (errno == EINTR));
        return NULL;
    }

    return fp;
}

/*
 ** Fetch a session with the given <sid> from the remote server.
 ** It is assumed that control connection has been opened already.
 */
FILE *
owp_fetch_sid(
        char        *savefile,
        OWPControl    cntrl,
        OWPSID        sid
        )
{
    char        *path;
    FILE        *fp;
    u_int32_t    num_rec;
    OWPErrSeverity    rc=OWPErrOK;

    /*
     * Prepare paths for datafiles. Unlink if not keeping data.
     */
    if(savefile){
        path = savefile;
        if( !(fp = fopen(path,"w+b"))){
            I2ErrLog(eh,"owp_fetch_sid:fopen(%s):%M",path);
            return NULL;
        }
    }
    else if( !(fp = tfile(eh))){
        return NULL;
    }

    /*
     * Ask for complete session 
     */
    num_rec = OWPFetchSession(cntrl,fp,0,(u_int32_t)0xFFFFFFFF,sid,&rc);
    if(!num_rec){
        if(path)
            (void)unlink(path);
        if(rc < OWPErrWARNING){
            I2ErrLog(eh,"owp_fetch_sid:OWPFetchSession error?");
            return NULL;
        }
        /*
         * server denied request...
         */
        I2ErrLog(eh,
                "owp_fetch_sid:Server denied request for to session data");
        return NULL;
    }

    return fp;
}

static OWPBoolean
getclientkey(
        OWPContext    ctx __attribute__((unused)),
        const OWPUserID    userid    __attribute__((unused)),
        OWPKey        key_ret,
        OWPErrSeverity    *err_ret __attribute__((unused))
        )
{
    memcpy(key_ret,aesbuff,sizeof(aesbuff));

    return True;
}

/*
 ** Initialize authentication and policy data (used by owping and owfetch)
 */
void
owp_set_auth(
        OWPContext    ctx,
        char        *progname,
        ow_ping_trec    *pctx
        )
{
    if(pctx->opt.identity){
        u_int8_t    *aes = NULL;

        /*
         * If keyfile specified, attempt to get key from there.
         */
        if(pctx->opt.keyfile){
            /* keyfile */
            FILE    *fp;
            int    rc = 0;
            char    *lbuf=NULL;
            size_t    lbuf_max=0;

            if(!(fp = fopen(pctx->opt.keyfile,"r"))){
                I2ErrLog(eh,"Unable to open %s: %M",
                        pctx->opt.keyfile);
                goto DONE;
            }

            rc = I2ParseKeyFile(eh,fp,0,&lbuf,&lbuf_max,NULL,
                    pctx->opt.identity,NULL,aesbuff);
            if(lbuf){
                free(lbuf);
            }
            lbuf = NULL;
            lbuf_max = 0;
            fclose(fp);

            if(rc > 0){
                aes = aesbuff;
            }
            else{
                I2ErrLog(eh,
                        "Unable to find key for id=\"%s\" from keyfile=\"%s\"",
                        pctx->opt.identity,pctx->opt.keyfile);
            }
        }else{
            /*
             * Do passphrase:
             *     open tty and get passphrase.
             *    (md5 the passphrase to create an aes key.)
             */
            char        *passphrase;
            char        ppbuf[MAX_PASSPHRASE];
            char        prompt[MAX_PASSPROMPT];
            I2MD5_CTX    mdc;
            size_t        pplen;

            if(snprintf(prompt,MAX_PASSPROMPT,
                        "Enter passphrase for identity '%s': ",
                        pctx->opt.identity) >= MAX_PASSPROMPT){
                I2ErrLog(eh,"ip_set_auth: Invalid identity");
                goto DONE;
            }

            if(!(passphrase = I2ReadPassPhrase(prompt,ppbuf,
                            sizeof(ppbuf),I2RPP_ECHO_OFF))){
                I2ErrLog(eh,"I2ReadPassPhrase(): %M");
                goto DONE;
            }
            pplen = strlen(passphrase);

            I2MD5Init(&mdc);
            I2MD5Update(&mdc,(unsigned char *)passphrase,pplen);
            I2MD5Final(aesbuff,&mdc);
            aes = aesbuff;
        }
DONE:
        if(aes){
            /*
             * install getaeskey func (key is in aesbuff)
             */
            OWPGetAESKeyFunc    getaeskey = getclientkey;

            if(!OWPContextConfigSetF(ctx,OWPGetAESKey,(OWPFunc)getaeskey)){
                I2ErrLog(eh,"Unable to set AESKey for context: %M");
                aes = NULL;
                goto DONE;
            }
        }
        else{
            free(pctx->opt.identity);
            pctx->opt.identity = NULL;
        }
    }


    /*
     * Verify/decode auth options.
     */
    if(pctx->opt.authmode){
        char    *s = ping_ctx.opt.authmode;
        pctx->auth_mode = 0;
        while(*s != '\0'){
            switch (toupper(*s)){
                case 'O':
                    pctx->auth_mode |= OWP_MODE_OPEN;
                    break;
                case 'A':
                    pctx->auth_mode |= OWP_MODE_AUTHENTICATED;
                    break;
                case 'E':
                    pctx->auth_mode |= OWP_MODE_ENCRYPTED;
                    break;
                default:
                    I2ErrLogP(eh,EINVAL,"Invalid -authmode %c",*s);
                    usage(progname, NULL);
                    exit(1);
            }
            s++;
        }
    }else{
        /*
         * Default to all modes.
         * If identity not set - library will ignore A/E.
         */
        pctx->auth_mode = OWP_MODE_OPEN|OWP_MODE_AUTHENTICATED|
            OWP_MODE_ENCRYPTED;
    }
}

/*
 * TODO: Find real max padding sizes based upon size of headers
 */
#define    MAX_PADDING_SIZE    65000

static OWPBoolean
parse_slots(
        char        *sched,
        OWPSlot        **slots_ret,
        u_int32_t    *nslots_ret
        )
{
    u_int32_t    i,nslots;
    char        *tstr;
    OWPSlot        *slots = NULL;

    if(!sched) return False;

    /*
     * count number of slots specified.
     */
    nslots=1;
    tstr=sched;
    while((tstr=strchr(tstr,','))){
        nslots++;
        tstr++;
    }

    /*
     * Allocate slot array.
     */
    if(!(slots = calloc(nslots,sizeof(OWPSlot)))){
        I2ErrLogP(eh,errno,"Can't alloc %d slots for schedule: %M",
                nslots);
        return False;
    }

    /*
     * parse string with strtok to grab each slot and fill the record
     */
    i=0;
    for(i=0,tstr = strtok(sched,",");
            (i<nslots) && tstr;
            tstr = strtok(NULL,","),i++){
        char    *endptr;
        double    dval;

        dval = strtod(tstr,&endptr);
        if(endptr == tstr){
            I2ErrLogP(eh,errno,
                    "Invalid numeric value (%s) for schedule",
                    tstr);
            goto FAILED;
        }
        if(strlen(endptr) > 1){
            I2ErrLogP(eh,errno,
                    "Invalid slot type (%s) for schedule",
                    endptr);
            goto FAILED;
        }
        switch(tolower(*endptr)){
            case '\0':
            case 'e':
                /* exponential slot */
                slots[i].slot_type = OWPSlotRandExpType;
                slots[i].rand_exp.mean = OWPDoubleToNum64(dval);
                break;
            case 'f':
                /* fixed offset slot */
                slots[i].slot_type = OWPSlotLiteralType;
                slots[i].literal.offset =OWPDoubleToNum64(dval);
                break;
            default:
                I2ErrLogP(eh,errno,
                        "Invalid slot type (%s) for schedule",
                        endptr);
                goto FAILED;
                break;
        }
    }

    if(i != nslots){
        I2ErrLogP(eh,errno,"Unable to parse schedule specification");
        goto FAILED;
    }

    *slots_ret = slots;
    *nslots_ret = nslots;
    return True;

FAILED:
    free(slots);
    return False;
}

static OWPBoolean
parse_ports(
        char        *pspec
        )
{
    char        *tstr,*endptr;
    long        tint;

    if(!pspec) return False;

    tstr = pspec;
    endptr = NULL;
    while(isspace(*tstr)) tstr++;
    tint = strtol(tstr,&endptr,10);
    if(!endptr || (tstr == endptr) || (tint < 0) || (tint > (int)0xffff)){
        goto FAILED;
    }
    ping_ctx.portrec.low = (u_int16_t)tint;

    while(isspace(*endptr)) endptr++;

    switch(*endptr){
        case '\0':
            /* only allow a single value if it is 0 */
            if(ping_ctx.portrec.low){
                goto FAILED;
            }
            ping_ctx.portrec.high = ping_ctx.portrec.low;
            goto DONE;
            break;
        case '-':
            endptr++;
            break;
        default:
            goto FAILED;
    }

    tstr = endptr;
    endptr = NULL;
    while(isspace(*tstr)) tstr++;
    tint = strtol(tstr,&endptr,10);
    if(!endptr || (tstr == endptr) || (tint < 0) || (tint > (int)0xffff)){
        goto FAILED;
    }
    ping_ctx.portrec.high = (u_int16_t)tint;

    if(ping_ctx.portrec.high < ping_ctx.portrec.low){
        goto FAILED;
    }

DONE:
    /*
     * If ephemeral is specified, shortcut but not setting.
     */
    if(!ping_ctx.portrec.high && !ping_ctx.portrec.low)
        return True;

    /*
     * Set.
     */
    ping_ctx.opt.portspec = &ping_ctx.portrec;
    return True;

FAILED:
    I2ErrLogP(eh,EINVAL,"Invalid port-range (-P): \"%s\": %M",pspec);
    return False;
}

int
main(
        int     argc,
        char    **argv
    )
{
    char                *progname;
    OWPErrSeverity      err_ret = OWPErrOK;
    I2LogImmediateAttr  ia;
    OWPContext          ctx;
    OWPTimeStamp        curr_time;
    OWPTestSpec         tspec;
    OWPSlot             slot;
    OWPNum64            rtt_bound;
    OWPNum64            delayStart;
    OWPSID              tosid, fromsid;
    OWPAcceptType       acceptval;
    OWPErrSeverity      err;
    FILE                *fromfp=NULL;
    char                localbuf[NI_MAXHOST+1+NI_MAXSERV+1];
    char                remotebuf[NI_MAXHOST+1+NI_MAXSERV+1];
    char                *local, *remote;

    int                 ch;
    char                *endptr = NULL;
    char                optstring[128];
    static char         *conn_opts = "A:k:S:u:";
    static char         *test_opts = "c:D:fF:H:i:L:P:s:tT:z:";
    static char         *out_opts = "a:QRv";
    static char         *gen_opts = "h";
#ifndef    NDEBUG
    static char         *debug_opts = "w";
#endif

    ia.line_info = (I2NAME | I2MSG);
#ifndef    NDEBUG
    ia.line_info |= (I2LINE | I2FILE);
#endif
    ia.fp = stderr;

    progname = (progname = strrchr(argv[0], '/')) ? progname+1 : *argv;

    /*
     * Start an error logging session for reporing errors to the
     * standard error
     */
    eh = I2ErrOpen(progname, I2ErrLogImmediate, &ia, NULL, NULL);
    if(! eh) {
        fprintf(stderr, "%s : Couldn't init error module\n", progname);
        exit(1);
    }

    if( (endptr = getenv("TMPDIR")))
        strncpy(tmpdir,endptr,PATH_MAX);
    else
        strncpy(tmpdir,_OWPING_DEF_TMPDIR,PATH_MAX);

    if(strlen(tmpdir) + strlen(_OWPING_PATH_SEPARATOR) +
            strlen(_OWPING_TMPFILEFMT) > PATH_MAX){
        I2ErrLog(eh, "TMPDIR too long");
        exit(1);
    }

    memset(&ping_ctx,0,sizeof(ping_ctx));

    /*
     * Initialize library with configuration functions.
     */
    if( !(ping_ctx.lib_ctx = OWPContextCreate(eh))){
        I2ErrLog(eh, "Unable to initialize OWP library.");
        exit(1);
    }
    ctx = ping_ctx.lib_ctx;

    /* Set default options. */
    ping_ctx.opt.records = ping_ctx.opt.childwait 
        = ping_ctx.opt.from = ping_ctx.opt.to = ping_ctx.opt.quiet
        = ping_ctx.opt.raw = False;
    ping_ctx.opt.save_from_test = ping_ctx.opt.save_to_test 
        = ping_ctx.opt.identity = ping_ctx.opt.keyfile 
        = ping_ctx.opt.srcaddr = ping_ctx.opt.authmode = NULL;
    ping_ctx.opt.numPackets = 100;
    ping_ctx.opt.lossThreshold = 0.0;
    ping_ctx.opt.delayStart = 0.0;
    ping_ctx.opt.percentile = 50.0;
    ping_ctx.opt.padding = 0;
    ping_ctx.mean_wait = (float)0.1;

    /* Create options strings for this program. */
    if (!strcmp(progname, "owping")) {
        strcpy(optstring, conn_opts);
        strcat(optstring, test_opts);
        strcat(optstring, out_opts);
    } else if (!strcmp(progname, "owstats")) {
        strcpy(optstring, out_opts);
    } else if (!strcmp(progname, "owfetch")) {
        strcpy(optstring, conn_opts);
        strcat(optstring, out_opts);
    }
    else{
        usage(progname, "Invalid program name.");
        exit(1);
    }

    strcat(optstring, gen_opts);
#ifndef    NDEBUG
    strcat(optstring,debug_opts);
#endif

    while((ch = getopt(argc, argv, optstring)) != -1){
        switch (ch) {
            u_int32_t   tlng;

            /* Connection options. */

            case 'A':
                if(!(ping_ctx.opt.authmode = strdup(optarg))){
                    I2ErrLog(eh,"malloc:%M");
                    exit(1);
                }
                break;
            case 'k':
                if (!(ping_ctx.opt.keyfile = strdup(optarg))){
                    I2ErrLog(eh,"malloc:%M");
                    exit(1);
                }
                break;
            case 'S':
                if(!(ping_ctx.opt.srcaddr = strdup(optarg))){
                    I2ErrLog(eh,"malloc:%M");
                    exit(1);
                }
                break;
            case 'u':
                if(!(ping_ctx.opt.identity = strdup(optarg))){
                    I2ErrLog(eh,"malloc:%M");
                    exit(1);
                }
                break;

                /* Test options. */

            case 'c':
                ping_ctx.opt.numPackets = strtoul(optarg, &endptr, 10);
                if (*endptr != '\0') {
                    usage(progname,
                            "Invalid value. Positive integer expected");
                    exit(1);
                }
                break;
            case 'D':
                if(ping_ctx.typeP){
                    usage(progname,
                            "Invalid option \'-D\'. Can only set one \'-D\' or \'-H\'.");
                    exit(1);
                }
                tlng = strtoul(optarg,&endptr,0);
                /*
                 * Validate int conversion and verify user only sets
                 * last 6 bits (DSCP must fit in 6 bits - RFC 2474.)
                 */
                if((*endptr != '\0') || (tlng & ~0x3F)){
                    usage(progname,
                            "Invalid value for option \'-D\'. DSCP value expected");
                    exit(1);
                }
                ping_ctx.typeP = tlng << 24;
                break;
            case 'F':
                if (!(ping_ctx.opt.save_from_test = strdup(optarg))){
                    I2ErrLog(eh,"malloc:%M");
                    exit(1);
                }
                /* fall through */
            case 'f':
                ping_ctx.opt.from = True;
                break;
            case 'H':
                if(ping_ctx.typeP){
                    usage(progname,
                            "Invalid option \'-H\'. Can only set one \'-H\' or \'-D\'.");
                    exit(1);
                }
                tlng = strtoul(optarg,&endptr,0);
                /*
                 * Validate int conversion and verify user only sets
                 * last 16 bits (PHB must fit in 16 bits - RFC 2836.)
                 */
                if((*endptr != '\0') || (tlng & ~0xFFFF)){
                    usage(progname,
                            "Invalid value for option \'-H\'. PHB value expected");
                    exit(1);
                }
                /* set second bit to specify PHB per owamp spec. */
                ping_ctx.typeP = 0x40000000;
                /* copy 16 bit PHB into next 16 bits of typeP. */
                ping_ctx.typeP |= (tlng << 14);
                break;
            case 'T':
                if (!(ping_ctx.opt.save_to_test = strdup(optarg))) {
                    I2ErrLog(eh,"malloc:%M");
                    exit(1);
                }
                /* fall through */
            case 't':
                ping_ctx.opt.to = True;
                break;
            case 'i':
                if(!parse_slots(optarg,&ping_ctx.slots,
                            &ping_ctx.nslots)){
                    usage(progname, "Invalid Schedule.");
                    exit(1);
                }
                break;
            case 's':
                ping_ctx.opt.padding = strtoul(optarg, &endptr, 10);
                if (*endptr != '\0') {
                    usage(progname, 
                            "Invalid value. Positive integer expected");
                    exit(1);
                }
                break;
            case 'L':
                ping_ctx.opt.lossThreshold = strtod(optarg,&endptr);
                if((*endptr != '\0') ||
                        (ping_ctx.opt.lossThreshold < 0.0)){
                    usage(progname, 
                            "Invalid \'-L\' value. Positive float expected");
                    exit(1);
                }
                break;
            case 'P':
                if(!parse_ports(optarg)){
                    usage(progname,
                            "Invalid test port range specified.");
                    exit(1);
                }
                break;
            case 'z':
                ping_ctx.opt.delayStart = strtod(optarg,&endptr);
                if((*endptr != '\0') ||
                        (ping_ctx.opt.delayStart < 0.0)){
                    usage(progname, 
                            "Invalid \'-z\' value. Positive float expected");
                    exit(1);
                }
                break;

                /* Output options */

            case 'v':
                ping_ctx.opt.records = True;
                break;
            case 'Q':
                ping_ctx.opt.quiet = True;
                break;
            case 'R':
                ping_ctx.opt.raw = True;
                break;
            case 'a':
                ping_ctx.opt.percentile =
                    (float)(strtod(optarg,&endptr));
                if ((*endptr != '\0')
                        || (ping_ctx.opt.percentile < 0.0) 
                        || (ping_ctx.opt.percentile > 100.0)){
                    usage(progname, 
                            "Invalid value. Floating number between 0.0 and 100.0 expected");
                    exit(1);
                }
                break;
#ifndef    NDEBUG
            case 'w':
                ping_ctx.opt.childwait = True;
                break;
#endif

                /* Generic options.*/
            case 'h':
            case '?':
            default:
                usage(progname, "");
                exit(0);
                /* UNREACHED */
        }
    }
    argc -= optind;
    argv += optind;

    if(ping_ctx.opt.raw){
        ping_ctx.opt.quiet = True;
    }

    /*
     * Handle 3 possible cases (owping, owfetch, owstats) one by one.
     */
    if (!strcmp(progname, "owping")){

        if((argc < 1) || (argc > 2)){
            usage(progname, NULL);
            exit(1);
        }

        if(!ping_ctx.opt.to && !ping_ctx.opt.from)
            ping_ctx.opt.to = ping_ctx.opt.from = True;

        ping_ctx.remote_test = argv[0];
        if(argc > 1)
            ping_ctx.remote_serv = argv[1];
        else
            ping_ctx.remote_serv = ping_ctx.remote_test;

        /*
         * This is in reality dependent upon the actual protocol used
         * (ipv4/ipv6) - it is also dependent upon the auth mode since
         * authentication implies 128bit block sizes.
         */
        if(ping_ctx.opt.padding > MAX_PADDING_SIZE)
            ping_ctx.opt.padding = MAX_PADDING_SIZE;

        owp_set_auth(ctx, progname, &ping_ctx); 

        /*
         * Determine schedule.
         */
        if(!ping_ctx.slots){
            tspec.nslots = 1;
            slot.slot_type = OWPSlotRandExpType;
            slot.rand_exp.mean = OWPDoubleToNum64(
                    ping_ctx.mean_wait);
            tspec.slots = &slot;
        }
        else{
            tspec.nslots = ping_ctx.nslots;
            tspec.slots = ping_ctx.slots;
        }

        /*
         * Setup test port range if specified.
         */
        if(ping_ctx.opt.portspec &&
                !OWPContextConfigSetV(ctx,OWPTestPortRange,
                    (void*)ping_ctx.opt.portspec)){
            I2ErrLog(eh,
                    "OWPContextConfigSetV(): Unable to set OWPTestPortRange?!");
            exit(1);
        }
#ifndef    NDEBUG
        /*
         * Setup debugging of child processes.
         */
        if(ping_ctx.opt.childwait &&
                !OWPContextConfigSetV(ctx,
                    OWPChildWait,
                    (void*)ping_ctx.opt.childwait)){
            I2ErrLog(eh,
                    "OWPContextConfigSetV(): Unable to set OWPChildWait?!");
        }
#endif

        /*
         * Open connection to owampd.
         */

        ping_ctx.cntrl = OWPControlOpen(ctx, 
                OWPAddrByNode(ctx, ping_ctx.opt.srcaddr),
                OWPAddrByNode(ctx, ping_ctx.remote_serv),
                ping_ctx.auth_mode,ping_ctx.opt.identity,
                NULL,&err_ret);
        if (!ping_ctx.cntrl){
            I2ErrLog(eh, "Unable to open control connection.");
            exit(1);
        }

        rtt_bound = OWPGetRTTBound(ping_ctx.cntrl);
        /*
         * Set the loss threshold to 2 seconds longer then the
         * rtt delay estimate. 2 is just a guess for a good number
         * based upon how impatient this command-line user gets for
         * results. Caveat: For the results to have any statistical
         * relevance the lossThreshold should be specified on the
         * command line. (You have to wait until this long after
         * the end of a test to declare the test over in order to
         * be confident that you have accepted all "duplicates"
         * that could come in during the test.)
         */
        if(ping_ctx.opt.lossThreshold <= 0.0){
            ping_ctx.opt.lossThreshold =
                OWPNum64ToDouble(rtt_bound) + 2.0;
        }

        /*
         * Compute a "min" start time.
         *
         * For now estimate a start time that allows both sides to
         * setup the session before that time:
         *     num_rtt * rtt_est + 1sec from now
         *
         *  There will be a rtt delay for the start sessions message
         *  and for each test request. For the default case, this
         *  will be 3 rtt's.
         */
        if(!OWPGetTimeOfDay(ctx,&curr_time)){
            I2ErrLogP(eh,errno,"Unable to get current time:%M");
            exit(1);
        }
        /* using ch to hold num_rtt */
        ch = 1;    /* startsessions command */
        if(ping_ctx.opt.to) ch++;
        if(ping_ctx.opt.from) ch++;
        tspec.start_time = OWPNum64Add(OWPNum64Mult(rtt_bound,
                                        OWPULongToNum64(ch)),
                                    OWPULongToNum64(1));

        /*
         * If the specified start time is greater than the "min"
         * start time, then use it.
         */
        if(ping_ctx.opt.delayStart > 0.0){
            delayStart = OWPDoubleToNum64(ping_ctx.opt.delayStart);
        }else{
            delayStart = OWPULongToNum64(0);
        }
        if(OWPNum64Cmp(delayStart,tspec.start_time) > 0){
            tspec.start_time = delayStart;
        }

        /*
         * Turn "relative" start time into an absolute time.
         */
        tspec.start_time = OWPNum64Add(tspec.start_time,curr_time.owptime);


        tspec.loss_timeout =
            OWPDoubleToNum64(ping_ctx.opt.lossThreshold);

        tspec.typeP = ping_ctx.typeP;
        tspec.packet_size_padding = ping_ctx.opt.padding;
        tspec.npackets = ping_ctx.opt.numPackets;


        /*
         * Prepare paths for datafiles. Unlink if not keeping data.
         */
        if(ping_ctx.opt.to) {
            if (!OWPSessionRequest(ping_ctx.cntrl, NULL, False,
                        OWPAddrByNode(ctx,ping_ctx.remote_test),
                        True,(OWPTestSpec*)&tspec,
                        NULL,tosid,&err_ret))
                FailSession(ping_ctx.cntrl);
        }

        if(ping_ctx.opt.from) {

            if (ping_ctx.opt.save_from_test) {
                fromfp = fopen(ping_ctx.opt.save_from_test,
                        "w+b");
                if(!fromfp){
                    I2ErrLog(eh,"fopen(%s):%M", 
                            ping_ctx.opt.save_from_test);
                    exit(1);
                }
            } else if( !(fromfp = tfile(eh))){
                exit(1);
            }

            if (!OWPSessionRequest(ping_ctx.cntrl,
                        OWPAddrByNode(ctx,ping_ctx.remote_test),
                        True, NULL, False,(OWPTestSpec*)&tspec,
                        fromfp,fromsid,&err_ret))
                FailSession(ping_ctx.cntrl);
        }


        if(OWPStartSessions(ping_ctx.cntrl) < OWPErrINFO)
            FailSession(ping_ctx.cntrl);

        /*
         * Give an estimate for when session data will be available.
         */
        if(!ping_ctx.opt.quiet){
            double  duration;
            double  rate;
            double  endtime;

            /*
             * First estimate duration of actual test session.
             */
            rate = OWPTestPacketRate(ctx,&tspec); 

            if(rate <= 0){
                duration = 0.0;
            }
            else{
                duration = (double)tspec.npackets / rate; 
            }

            /*
             * Now wait lossThreshold for duplicate packet detection.
             */
            duration += ping_ctx.opt.lossThreshold;

            /*
             * Now wait for StopSessions messages to be exchanged.
             */
            duration += OWPNum64ToDouble(rtt_bound);

            /*
             * Compute "endtime" based on starttime and duration
             */
            endtime = OWPNum64ToDouble(tspec.start_time) + duration;

            /*
             * Compute a relative time from "endtime" and curr_time.
             */
            if(!OWPGetTimeOfDay(ctx,&curr_time)){
                I2ErrLogP(eh,errno,"Unable to get current time:%M");
                exit(1);
            }

            endtime -= OWPNum64ToDouble(curr_time.owptime);

            fprintf(stdout,
                    "Approximately %.1f seconds until results available\n",
                    endtime);
        }

        /*
         * TODO install sig handler for keyboard interupt - to send 
         * stop sessions. (Currently SIGINT causes everything to be 
         * killed and lost - might be reasonable to keep it that
         * way...)
         */
        if(OWPStopSessionsWait(ping_ctx.cntrl,NULL,NULL,&acceptval,
                    &err)){
            exit(1);
        }

        if (acceptval != 0) {
            I2ErrLog(eh, "Test session(s) Failed...");

            exit(0);
        }

        /*
         * Get "local" and "remote" names for pretty printing
         * if we need them.
         */
        local = remote = NULL;
        if(!ping_ctx.opt.quiet){
            OWPAddr    laddr;
            size_t    lsize;

            /*
             * First determine local address.
             */
            if(ping_ctx.opt.srcaddr){
                laddr = OWPAddrByNode(ctx,
                        ping_ctx.opt.srcaddr);
            }
            else{
                laddr = OWPAddrByLocalControl(
                        ping_ctx.cntrl);
            }
            lsize = sizeof(localbuf);
            OWPAddrNodeName(laddr,localbuf,&lsize);
            if(lsize > 0){
                local = localbuf;
            }
            OWPAddrFree(laddr);

            /*
             * Now determine remote address.
             */
            laddr = OWPAddrByNode(ctx,ping_ctx.remote_test);
            lsize = sizeof(remotebuf);
            OWPAddrNodeName(laddr,remotebuf,&lsize);
            if(lsize > 0){
                remote = remotebuf;
            }
            OWPAddrFree(laddr);
        }

        if(ping_ctx.opt.to && (ping_ctx.opt.save_to_test ||
                    !ping_ctx.opt.quiet || ping_ctx.opt.raw)){
            FILE    *tofp;

            tofp = owp_fetch_sid(ping_ctx.opt.save_to_test,
                    ping_ctx.cntrl,tosid);
            if(tofp && (!ping_ctx.opt.quiet || ping_ctx.opt.raw) &&
                    (do_records_all(ctx,stdout,tofp,
                                    local,remote) < 0)){
                I2ErrLog(eh,
                        "do_records_all(\"to\" session): %M");
            }
            if(tofp && fclose(tofp)){
                I2ErrLog(eh,"close(): %M");
            }
        }

        if(fromfp && (!ping_ctx.opt.quiet || ping_ctx.opt.raw)){
            if(do_records_all(ctx,stdout,fromfp,remote,local)
                    < 0){
                I2ErrLog(eh,
                        "do_records_all(\"from\" session): %M");
            }
        }

        if(fromfp && fclose(fromfp)){
            I2ErrLog(eh,"close(): %M");
        }

        exit(0);

    }

    if (!strcmp(progname, "owstats")) {
        FILE        *fp;

        if(!(fp = fopen(argv[0],"rb"))){
            I2ErrLog(eh,"fopen(%s):%M",argv[0]);
            exit(1);
        }

        if (do_records_all(ctx,stdout,fp,NULL,NULL) < 0){
            I2ErrLog(eh,"do_records_all() failed.");
            exit(1);
        }

        fclose(fp);

        exit(0);
    }

    if (!strcmp(progname, "owfetch")) {
        int i;
        if((argc%2 == 0) || (argc < 3)){
            usage(progname, NULL);
            exit(1);
        }

        ping_ctx.remote_serv = argv[0];
        argv++;
        argc--;

        owp_set_auth(ctx, progname, &ping_ctx); 

        /*
         * Open connection to owampd.
         */
        ping_ctx.cntrl = OWPControlOpen(ctx, 
                OWPAddrByNode(ctx, ping_ctx.opt.srcaddr),
                OWPAddrByNode(ctx, ping_ctx.remote_serv),
                ping_ctx.auth_mode,ping_ctx.opt.identity,
                NULL,&err_ret);
        if (!ping_ctx.cntrl){
            I2ErrLog(eh, "Unable to open control connection.");
            exit(1);
        }

        for (i = 0; i < argc/2; i++) {
            OWPSID    sid;
            FILE    *fp;
            char    *sname;
            char    *fname;

            sname = *argv++;
            fname = *argv++;
            I2HexDecode(sname, sid, 16);
            if(!(fp = owp_fetch_sid(fname,ping_ctx.cntrl,sid))){
                I2ErrLog(eh,"Unable to fetch sid(%s)",sname);
            }
            else if(!ping_ctx.opt.quiet &&
                    do_records_all(ctx,stdout,fp,NULL,NULL)
                    < 0){
                I2ErrLog(eh,"do_records_all() failed.");
            }
            else if(fclose(fp)){
                I2ErrLog(eh,"fclose(): %M");
            }
        }

        exit(0);
    }

    exit(0);
}
