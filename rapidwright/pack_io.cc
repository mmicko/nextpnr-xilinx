/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <david@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "pack.h"
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

CellInfo *USPacker::insert_ibufctrl(IdString name, NetInfo *i, NetInfo *o)
{
    auto ibufc = create_cell(ctx, ctx->id("IBUFCTRL"), name);
    connect_port(ctx, i, ibufc.get(), ctx->id("I"));
    connect_port(ctx, o, ibufc.get(), ctx->id("O"));
    CellInfo *ibufc_ptr = ibufc.get();
    new_cells.push_back(std::move(ibufc));
    return ibufc_ptr;
}

CellInfo *USPacker::insert_inbuf(IdString name, NetInfo *pad, NetInfo *o)
{
    auto inbuf = create_cell(ctx, ctx->id("INBUF"), name);
    connect_port(ctx, pad, inbuf.get(), ctx->id("PAD"));
    connect_port(ctx, o, inbuf.get(), ctx->id("O"));
    CellInfo *inbuf_ptr = inbuf.get();
    new_cells.push_back(std::move(inbuf));
    return inbuf_ptr;
}

CellInfo *USPacker::insert_obuf(IdString name, IdString type, NetInfo *i, NetInfo *o, NetInfo *tri)
{
    auto obuf = create_cell(ctx, type, name);
    connect_port(ctx, i, obuf.get(), ctx->id("I"));
    connect_port(ctx, tri, obuf.get(), ctx->id("TRI"));
    connect_port(ctx, o, obuf.get(), ctx->id("O"));
    CellInfo *obuf_ptr = obuf.get();
    new_cells.push_back(std::move(obuf));
    return obuf_ptr;
}

CellInfo *USPacker::insert_diffinbuf(IdString name, NetInfo *i[2], NetInfo *o)
{
    auto dibuf = create_cell(ctx, ctx->id("DIFFINBUF"), name);
    connect_port(ctx, i[0], dibuf.get(), ctx->id("DIFF_IN_P"));
    connect_port(ctx, i[1], dibuf.get(), ctx->id("DIFF_IN_N"));
    connect_port(ctx, o, dibuf.get(), ctx->id("O"));
    CellInfo *dibuf_ptr = dibuf.get();
    new_cells.push_back(std::move(dibuf));
    return dibuf_ptr;
}

CellInfo *USPacker::create_iobuf(CellInfo *npnr_io, IdString &top_port)
{
    std::unique_ptr<CellInfo> cell;
    if (npnr_io->type == ctx->id("$nextpnr_ibuf")) {
        cell = create_cell(ctx, ctx->id("IBUF"), ctx->id(npnr_io->name.str(ctx) + "$ibuf$"));
        replace_port(npnr_io, ctx->id("O"), cell.get(), ctx->id("O"));
        top_port = ctx->id("I");
    }
    CellInfo *tbuf = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_obuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        NetInfo *donet = npnr_io->ports.at(ctx->id("I")).net;
        tbuf = net_driven_by(
                ctx, donet, [](const Context *ctx, const CellInfo *cell) { return cell->type == ctx->id("$_TBUF_"); },
                ctx->id("Y"));

        if (npnr_io->type == ctx->id("$nextpnr_obuf")) {
            cell = create_cell(ctx, tbuf ? ctx->id("OBUFT") : ctx->id("OBUF"),
                               ctx->id(npnr_io->name.str(ctx) + "$obuf$"));
            top_port = ctx->id("O");
        } else {
            cell = create_cell(ctx, ctx->id("IOBUF"), ctx->id(npnr_io->name.str(ctx) + "$iobuf$"));
            replace_port(npnr_io, ctx->id("O"), cell.get(), ctx->id("O"));
            top_port = ctx->id("IO");
        }
    }
    CellInfo *iob_ptr = cell.get();
    new_cells.push_back(std::move(cell));
    return iob_ptr;
}

std::pair<CellInfo *, PortRef> USPacker::insert_pad_and_buf(CellInfo *npnr_io)
{
    // Given a nextpnr IO buffer, create a PAD instance and insert an IO buffer if one isn't already present
    std::pair<CellInfo *, PortRef> result;
    auto pad_cell = create_cell(ctx, ctx->id("PAD"), npnr_io->name);
    // Copy IO attributes to pad
    for (auto &attr : npnr_io->attrs)
        pad_cell->attrs[attr.first] = attr.second;
    NetInfo *ionet = nullptr;
    PortRef iobuf;
    iobuf.cell = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_ibuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = get_net_or_empty(npnr_io, ctx->id("O"));
        if (ionet != nullptr)
            for (auto &usr : ionet->users)
                if (toplevel_ports.count(usr.cell->type) && toplevel_ports.at(usr.cell->type).count(usr.port)) {
                    if (ionet->users.size() > 1)
                        log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                                  usr.cell->name.c_str(ctx));
                    iobuf = usr;
                }
    }
    if (npnr_io->type == ctx->id("$nextpnr_ibuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = get_net_or_empty(npnr_io, ctx->id("I"));
        if (ionet != nullptr && ionet->driver.cell != nullptr)
            if (toplevel_ports.count(ionet->driver.cell->type) &&
                toplevel_ports.at(ionet->driver.cell->type).count(ionet->driver.port)) {
                if (ionet->users.size() > 1)
                    log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                              ionet->driver.cell->name.c_str(ctx));
                iobuf = ionet->driver;
            }
    }

    if (!iobuf.cell) {
        // No IO buffer, need to create one
        if (npnr_io->type == ctx->id("$nextpnr_ibuf")) {
        }
    }

    result.first = pad_cell.get();
    result.second = iobuf;
    packed_cells.insert(npnr_io->name);
    new_cells.push_back(std::move(pad_cell));
    return result;
}

void USPacker::pack_io()
{
    log_info("Inserting IO buffers..\n");
    std::unordered_map<IdString, XFormRule> io_rules;
    io_rules[ctx->id("$nextpnr_ibuf")].new_type = ctx->id("IOB_IBUFCTRL");
    io_rules[ctx->id("$nextpnr_ibuf")].set_attrs.emplace_back(ctx->id("X_ORIG_TYPE"), "IBUFCTRL");

    io_rules[ctx->id("$nextpnr_obuf")].new_type = ctx->id("IOB_OUTBUF");
    io_rules[ctx->id("$nextpnr_obuf")].set_attrs.emplace_back(ctx->id("X_ORIG_TYPE"), "OBUF");

    io_rules[ctx->id("BUFGCTRL")].new_type = ctx->id("BUFGCTRL");
    io_rules[ctx->id("OBUF")].new_type = ctx->id("IOB_OUTBUF");
    io_rules[ctx->id("INBUF")].new_type = ctx->id("IOB_INBUF");
    io_rules[ctx->id("IBUFCTRL")].new_type = ctx->id("IOB_IBUFCTRL");

    io_rules[ctx->id("PS8")].new_type = ctx->id("PSS_ALTO_CORE");
    io_rules[ctx->id("BUFG_PS")].new_type = ctx->id("BUFCE_BUFG_PS");

    generic_xform(io_rules, true);

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_IOB_IBUFCTRL || ci->type == id_IOB_OUTBUF) {
            if (ci->attrs.count(ctx->id("LOC"))) {
                std::string loc = ci->attrs.at(ctx->id("LOC"));
                std::string site = ctx->getPackagePinSite(loc);
                if (site.empty())
                    log_error("Unable to constrain IO '%s', device does not have a pin named '%s'\n",
                              ci->name.c_str(ctx), loc.c_str());
                log_info("    Constraining '%s' to site '%s'\n", ci->name.c_str(ctx), site.c_str());
                std::string belname = (ci->type == id_IOB_IBUFCTRL) ? "IBUFCTRL" : "OUTBUF";
                ci->attrs[ctx->id("BEL")].setString(site + "/" + belname);
            }
        }

        if (ci->type == id_IOB_OUTBUF) {
            NetInfo *inet = get_net_or_empty(ci, ctx->id("I"));
            if (inet)
                rename_net(inet->name, ctx->id(inet->name.str(ctx) + "$obuf_I$"));
        } else if (ci->type == id_IOB_IBUFCTRL) {
            NetInfo *onet = get_net_or_empty(ci, ctx->id("O"));
            if (onet)
                rename_net(onet->name, ctx->id(onet->name.str(ctx) + "$ibuf_O$"));
        }
    }
}

NEXTPNR_NAMESPACE_END