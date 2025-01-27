/* packet-mctp.c
 * Routines for Management Component Transport Protocol (MCTP) packet
 * disassembly
 * Copyright 2022, Jeremy Kerr <jk@codeconstruct.com.au>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * MCTP is a datagram-based protocol for intra-platform communication,
 * typically between a management controller and system devices.
 *
 * MCTP is defined by DMTF standard DSP0236: https://www.dmtf.org/dsp/DSP0236
 */

#include <config.h>

#include <epan/packet.h>
#include <epan/reassemble.h>
#include <epan/to_str.h>
#include <epan/dissectors/packet-sll.h>

#define MCTP_MIN_LENGTH 5       /* 4-byte header, plus message type */

void proto_register_mctp(void);
void proto_reg_handoff_mctp(void);

static int proto_mctp = -1;

static int hf_mctp_ver = -1;
static int hf_mctp_dst = -1;
static int hf_mctp_src = -1;
static int hf_mctp_flags = -1;
static int hf_mctp_flags_som = -1;
static int hf_mctp_flags_eom = -1;
static int hf_mctp_seq = -1;
static int hf_mctp_tag = -1;
static int hf_mctp_tag_to = -1;
static int hf_mctp_tag_value = -1;

static gint ett_mctp = -1;
static gint ett_mctp_fst = -1;
static gint ett_mctp_flags = -1;
static gint ett_mctp_tag = -1;

static const true_false_string tfs_tag_to = { "Sender", "Receiver" };

static int hf_mctp_fragments = -1;
static int hf_mctp_fragment = -1;
static int hf_mctp_fragment_overlap = -1;
static int hf_mctp_fragment_overlap_conflicts = -1;
static int hf_mctp_fragment_multiple_tails = -1;
static int hf_mctp_fragment_too_long_fragment = -1;
static int hf_mctp_fragment_error = -1;
static int hf_mctp_fragment_count = -1;
static int hf_mctp_reassembled_in = -1;
static int hf_mctp_reassembled_length = -1;
static int hf_mctp_reassembled_data = -1;

static gint ett_mctp_fragment = -1;
static gint ett_mctp_fragments = -1;

static const fragment_items mctp_frag_items = {
    /* Fragment subtrees */
    &ett_mctp_fragment,
    &ett_mctp_fragments,
    /* Fragment fields */
    &hf_mctp_fragments,
    &hf_mctp_fragment,
    &hf_mctp_fragment_overlap,
    &hf_mctp_fragment_overlap_conflicts,
    &hf_mctp_fragment_multiple_tails,
    &hf_mctp_fragment_too_long_fragment,
    &hf_mctp_fragment_error,
    &hf_mctp_fragment_count,
    /* "Reassembled in" field */
    &hf_mctp_reassembled_in,
    /* Reassembled length field */
    &hf_mctp_reassembled_length,
    &hf_mctp_reassembled_data,
    /* Tag */
    "Message fragments"
};

static const value_string flag_vals[] = {
    { 0x00, "none" },
    { 0x01, "EOM" },
    { 0x02, "SOM" },
    { 0x03, "SOM|EOM" },
    { 0x00, NULL },
};

static dissector_table_t mctp_dissector_table;
static reassembly_table mctp_reassembly_table;

static int
dissect_mctp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
        void *data _U_)
{
    proto_tree *mctp_tree, *fst_tree;
    guint len, ver, type, seq, fst;
    bool save_fragmented;
    proto_item *ti, *tti;
    tvbuff_t *next_tvb;
    guint8 tag;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MCTP");
    col_clear(pinfo->cinfo, COL_INFO);

    /* Check that the packet is long enough for it to belong to us. */
    len = tvb_reported_length(tvb);

    if (len < MCTP_MIN_LENGTH) {
        col_add_fstr(pinfo->cinfo, COL_INFO, "Bogus length %u, minimum %u",
                     len, MCTP_MIN_LENGTH);
        return tvb_captured_length(tvb);
    }

    ver = tvb_get_bits8(tvb, 4, 4);
    if (ver != 1) {
        col_add_fstr(pinfo->cinfo, COL_INFO, "Invalid version %u", ver);
        return tvb_captured_length(tvb);
    }

    /* Top-level protocol item & tree */
    ti = proto_tree_add_item(tree, proto_mctp, tvb, 0, 4, ENC_NA);
    mctp_tree = proto_item_add_subtree(ti, ett_mctp);

    set_address_tvb(&pinfo->dl_dst, AT_MCTP, 1, tvb, 1);
    set_address_tvb(&pinfo->dl_src, AT_MCTP, 1, tvb, 2);
    copy_address_shallow(&pinfo->dst, &pinfo->dl_dst);
    copy_address_shallow(&pinfo->src, &pinfo->dl_src);

    proto_item_append_text(ti, " Dst: %s, Src %s",
            address_to_str(pinfo->pool, &pinfo->dst),
            address_to_str(pinfo->pool, &pinfo->src));

    /* Standard header fields */
    proto_tree_add_item(mctp_tree, hf_mctp_ver, tvb, 0, 1, ENC_NA);
    proto_tree_add_item(mctp_tree, hf_mctp_dst, tvb, 1, 1, ENC_NA);
    proto_tree_add_item(mctp_tree, hf_mctp_src, tvb, 2, 1, ENC_NA);

    static int * const mctp_flags[] = {
        &hf_mctp_flags_som,
        &hf_mctp_flags_eom,
        NULL
    };

    static int * const mctp_tag[] = {
        &hf_mctp_tag_to,
        &hf_mctp_tag_value,
        NULL,
    };

    fst = tvb_get_guint8(tvb, 3);
    tag = fst & 0x0f;
    fst_tree = proto_tree_add_subtree_format(mctp_tree, tvb, 3, 1, ett_mctp_fst,
                                      &tti, "Flags %s, seq %d, tag %s%d",
                                      val_to_str_const(fst >> 6, flag_vals, ""),
                                      fst >> 4 & 0x3,
                                      fst & 0x08 ? "TO:" : "",
                                      fst & 0x7);
    proto_tree_add_bitmask(fst_tree, tvb, 3, hf_mctp_flags,
                           ett_mctp_flags, mctp_flags, ENC_NA);
    proto_tree_add_item_ret_uint(fst_tree, hf_mctp_seq, tvb, 3, 1, ENC_NA, &seq);
    proto_tree_add_bitmask_with_flags(fst_tree, tvb, 3, hf_mctp_tag,
                           ett_mctp_tag, mctp_tag, ENC_NA, BMT_NO_FLAGS);

    /* use the tags as our port numbers */
    pinfo->ptype = PT_MCTP;
    pinfo->srcport = tag;
    pinfo->destport = tag ^ 0x08; /* flip tag-owner bit */

    save_fragmented = pinfo->fragmented;

    col_set_str(pinfo->cinfo, COL_INFO, "MCTP message");

    /* if we're not both the start and end of a message, handle as a
     * fragment */
    if ((fst & 0xc0) != 0xc0) {
        fragment_head *frag_msg = NULL;
        tvbuff_t *new_tvb = NULL;

        pinfo->fragmented = true;
        frag_msg = fragment_add_seq_next(&mctp_reassembly_table,
                                         tvb, 4, pinfo,
                                         fst & 0x7, NULL,
                                         tvb_captured_length_remaining(tvb, 4),
                                         !(fst & 0x40));

        new_tvb = process_reassembled_data(tvb, 4, pinfo,
                                           "reassembled Message",
                                           frag_msg, &mctp_frag_items,
                                           NULL, mctp_tree);

        if (fst & 0x40)
            col_append_str(pinfo->cinfo, COL_INFO, " reassembled");
        else
            col_append_fstr(pinfo->cinfo, COL_INFO, " frag %u", seq);

        next_tvb = new_tvb;
    } else {
        next_tvb = tvb_new_subset_remaining(tvb, 4);
    }

    if (next_tvb) {
        type = tvb_get_guint8(next_tvb, 0);
        dissector_try_uint_new(mctp_dissector_table, type & 0x7f, next_tvb,
                               pinfo, tree, true, NULL);
    }

    pinfo->fragmented = save_fragmented;

    return tvb_captured_length(tvb);
}

void
proto_register_mctp(void)
{
    /* *INDENT-OFF* */
    /* Field definitions */
    static hf_register_info hf[] = {
        { &hf_mctp_ver,
          { "Version", "mctp.version",
            FT_UINT8, BASE_DEC, NULL, 0x0f,
            NULL, HFILL },
        },
        { &hf_mctp_dst,
          { "Destination", "mctp.dst",
            FT_UINT8, BASE_DEC, NULL, 0x00,
            NULL, HFILL },
        },
        { &hf_mctp_src,
          { "Source", "mctp.src",
            FT_UINT8, BASE_DEC, NULL, 0x00,
            NULL, HFILL },
        },
        { &hf_mctp_flags,
          { "Flags", "mctp.flags",
            FT_UINT8, BASE_HEX, NULL, 0xc0,
            NULL, HFILL },
        },
        { &hf_mctp_flags_som,
          { "Start of message", "mctp.flags.som",
            FT_BOOLEAN, 8, TFS(&tfs_set_notset), 0x80,
            NULL, HFILL },
        },
        { &hf_mctp_flags_eom,
          { "End of message", "mctp.flags.eom",
            FT_BOOLEAN, 8, TFS(&tfs_set_notset), 0x40,
            NULL, HFILL },
        },
        { &hf_mctp_seq,
          { "Sequence", "mctp.seq",
            FT_UINT8, BASE_HEX, NULL, 0x30,
            NULL, HFILL },
        },
        { &hf_mctp_tag,
          { "Tag", "mctp.tag",
            FT_UINT8, BASE_HEX, NULL, 0x0f,
            NULL, HFILL },
        },
        { &hf_mctp_tag_to,
          { "Tag owner", "mctp.tag.to",
            FT_BOOLEAN, 8, TFS(&tfs_tag_to), 0x08,
            NULL, HFILL },
        },
        { &hf_mctp_tag_value,
          { "Tag value", "mctp.tag.value",
            FT_UINT8, BASE_HEX, NULL, 0x07,
            NULL, HFILL },
        },

        /* generic fragmentation */
        {&hf_mctp_fragments,
            {"Message fragments", "mctp.fragments",
                FT_NONE, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment,
            {"Message fragment", "mctp.fragment",
                FT_FRAMENUM, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment_overlap,
            {"Message fragment overlap", "mctp.fragment.overlap",
                FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment_overlap_conflicts,
            {"Message fragment overlapping with conflicting data",
                "mctp.fragment.overlap.conflicts",
                FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment_multiple_tails,
            {"Message has multiple tail fragments",
                "mctp.fragment.multiple_tails",
                FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment_too_long_fragment,
            {"Message fragment too long", "mctp.fragment.too_long_fragment",
                FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment_error,
            {"Message defragmentation error", "mctp.fragment.error",
                FT_FRAMENUM, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_fragment_count,
            {"Message fragment count", "mctp.fragment.count",
                FT_UINT32, BASE_DEC, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_reassembled_in,
            {"Reassembled in", "mctp.reassembled.in",
                FT_FRAMENUM, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_reassembled_length,
            {"Reassembled length", "mctp.reassembled.length",
                FT_UINT32, BASE_DEC, NULL, 0x00, NULL, HFILL } },
        {&hf_mctp_reassembled_data,
            {"Reassembled data", "mctp.reassembled.data",
                FT_BYTES, SEP_SPACE, NULL, 0x00, NULL, HFILL } },
    };

    /* protocol subtree */
    static gint *ett[] = {
        &ett_mctp,
        &ett_mctp_flags,
        &ett_mctp_fst,
        &ett_mctp_tag,
        &ett_mctp_fragment,
        &ett_mctp_fragments,
    };

    /* Register the protocol name and description */
    proto_mctp = proto_register_protocol("MCTP", "MCTP", "mctp");

    /* Required function calls to register the header fields and subtrees */
    proto_register_field_array(proto_mctp, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    mctp_dissector_table = register_dissector_table("mctp.type", "MCTP type",
                                                    proto_mctp, FT_UINT8,
                                                    BASE_HEX);

    reassembly_table_register(&mctp_reassembly_table,
                              &addresses_reassembly_table_functions);
}

void
proto_reg_handoff_mctp(void)
{
    dissector_handle_t mctp_handle;
    mctp_handle = create_dissector_handle(dissect_mctp, proto_mctp);
    dissector_add_uint("sll.ltype", LINUX_SLL_P_MCTP, mctp_handle);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
