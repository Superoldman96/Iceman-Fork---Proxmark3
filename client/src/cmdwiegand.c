//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Wiegand commands
//-----------------------------------------------------------------------------
#include "cmdwiegand.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "cmdparser.h"          // command_t
#include "cliparser.h"
#include "comms.h"
#include "pm3_cmd.h"
#include "protocols.h"
#include "parity.h"             // oddparity
#include "cmdhflist.h"          // annotations
#include "wiegand_formats.h"
#include "wiegand_formatutils.h"
#include "util.h"

static int CmdHelp(const char *Cmd);

#define PACS_EXTRA_LONG_FORMAT  18     // 144 bits
#define PACS_LONG_FORMAT        12     // 96 bits
#define PACS_FORMAT             6      // 44 bits
static int wiegand_new_pacs(uint8_t *padded_pacs, uint8_t plen) {

    uint8_t d[PACS_EXTRA_LONG_FORMAT] = {0};
    memcpy(d, padded_pacs, plen);

    uint8_t pad = d[0];

    char *binstr = (char *)calloc((PACS_EXTRA_LONG_FORMAT * 8) + 1, sizeof(uint8_t));
    if (binstr == NULL) {
        PrintAndLogEx(WARNING, "Failed to allocate memory");
        return PM3_EMALLOC;
    }

    uint8_t n = plen - 1;

    bytes_2_binstr(binstr, d + 1, n);

    binstr[strlen(binstr) - pad] = '\0';

    size_t tlen = 0;
    uint8_t tmp[16] = {0};
    binstr_2_bytes(tmp, &tlen, binstr);
    PrintAndLogEx(SUCCESS, "Wiegand raw.... " _YELLOW_("%s"), sprint_hex_inrow(tmp, tlen));

    uint32_t top = 0, mid = 0, bot = 0;
    if (binstring_to_u96(&top, &mid, &bot, binstr) != strlen(binstr)) {
        PrintAndLogEx(ERR, "Binary string contains none <0|1> chars");
        free(binstr);
        return PM3_EINVARG;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "------------------------- " _CYAN_("SIO - Wiegand") " ---------------------------");
    decode_wiegand(top, mid, bot, strlen(binstr));
    free(binstr);
    return PM3_SUCCESS;
}
int CmdWiegandList(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "wiegand info",
                  "List available wiegand formats",
                  "wiegand list"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    HIDListFormats();
    return PM3_SUCCESS;
}

int CmdWiegandEncode(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "wiegand encode",
                  "Encode wiegand formatted number to raw hex",
                  "wiegand encode --fc 101 --cn 1337               ->  show all formats\n"
                  "wiegand encode -w H10301 --fc 101 --cn 1337     ->  H10301 format "
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_u64_0(NULL, "fc", "<dec>", "facility number"),
        arg_u64_1(NULL, "cn", "<dec>", "card number"),
        arg_u64_0(NULL, "issue", "<dec>", "issue level"),
        arg_u64_0(NULL, "oem", "<dec>", "OEM code"),
        arg_str0("w", "wiegand", "<format>", "see `wiegand list` for available formats"),
        arg_lit0(NULL, "pre", "add HID ProxII preamble to wiegand output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    wiegand_card_t data;
    memset(&data, 0, sizeof(wiegand_card_t));

    data.FacilityCode = arg_get_u32_def(ctx, 1, 0);
    data.CardNumber = arg_get_u64_def(ctx, 2, 0);
    data.IssueLevel = arg_get_u32_def(ctx, 3, 0);
    data.OEM = arg_get_u32_def(ctx, 4, 0);

    int len = 0;
    char format[16] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 5), (uint8_t *)format, sizeof(format), &len);
    bool preamble = arg_get_lit(ctx, 6);
    CLIParserFree(ctx);

    int idx = -1;
    if (len) {
        idx = HIDFindCardFormat(format);
        if (idx == -1) {
            PrintAndLogEx(WARNING, "Unknown format: %s", format);
            return PM3_EINVARG;
        }
    }

    if (idx != -1) {
        wiegand_message_t packed;
        memset(&packed, 0, sizeof(wiegand_message_t));
        if (HIDPack(idx, &data, &packed, preamble) == false) {
            PrintAndLogEx(WARNING, "The card data could not be encoded in the selected format.");
            return PM3_ESOFT;
        }
        print_wiegand_code(&packed);
    } else {
        // try all formats and print only the ones that work.
        HIDPackTryAll(&data, preamble);
    }
    return PM3_SUCCESS;
}

int CmdWiegandDecode(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "wiegand decode",
                  "Decode raw hex or binary to wiegand format",
                  "wiegand decode --raw 2006F623AE\n"
                  "wiegand decode --new 06BD88EB80   -> 4..8 bytes, new padded format "
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_str0("r", "raw", "<hex>", "raw hex to be decoded"),
        arg_str0("b", "bin", "<bin>", "binary string to be decoded"),
        arg_str0("n", "new", "<hex>", "new padded pacs as raw hex to be decoded"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int hlen = 0;
    char hex[40] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)hex, sizeof(hex), &hlen);

    int blen = 0;
    uint8_t binarr[100] = {0x00};
    int res = CLIParamBinToBuf(arg_get_str(ctx, 2), binarr, sizeof(binarr), &blen);

    int plen = 0;
    uint8_t phex[8] = {0};
    res = CLIParamHexToBuf(arg_get_str(ctx, 3), phex, sizeof(phex), &plen);

    CLIParserFree(ctx);

    if (res) {
        PrintAndLogEx(FAILED, "Error parsing binary string");
        return PM3_EINVARG;
    }

    uint32_t top = 0, mid = 0, bot = 0;

    if (hlen) {
        res = hexstring_to_u96(&top, &mid, &bot, hex);
        if (res != hlen) {
            PrintAndLogEx(ERR, "Hex string contains none hex chars");
            return PM3_EINVARG;
        }
    } else if (blen) {
        int n = binarray_to_u96(&top, &mid, &bot, binarr, blen);
        if (n != blen) {
            PrintAndLogEx(ERR, "Binary string contains none <0|1> chars");
            return PM3_EINVARG;
        }
        PrintAndLogEx(INFO, "#bits... %d", blen);

    } else if (plen) {

        return wiegand_new_pacs(phex, plen);

    } else {
        PrintAndLogEx(ERR, "Empty input");
        return PM3_EINVARG;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "------------------------- " _CYAN_("Wiegand") " ---------------------------");

    decode_wiegand(top, mid, bot, blen);
    return PM3_SUCCESS;
}

static command_t CommandTable[] = {
    {"help",    CmdHelp,           AlwaysAvailable, "This help"},
    {"list",    CmdWiegandList,    AlwaysAvailable, "List available wiegand formats"},
    {"encode",  CmdWiegandEncode,  AlwaysAvailable, "Encode to wiegand raw hex (currently for HID Prox)"},
    {"decode",  CmdWiegandDecode,  AlwaysAvailable, "Convert raw hex to decoded wiegand format (currently for HID Prox)"},
    {NULL, NULL, NULL, NULL}
};

static int CmdHelp(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}

int CmdWiegand(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}
