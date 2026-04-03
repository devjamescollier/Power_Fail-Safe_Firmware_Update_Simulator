#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_sim.h"
#include "fwu/update.h"

enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2, EXIT_POWER_LOSS = 3 };

#define DEFAULT_FLASH_PATH "flash.img"

#define FILE_CHUNK 1000u

static void usage(FILE *out)
{
    fprintf(out,
            "usage: fwsim [--flash PATH] [--fail-at N] <command> [args]\n"
            "commands:\n"
            "  init <factory.img>   create flash with a confirmed factory image\n"
            "  status               show metadata and slot validity\n"
            "  install <image>      stage a candidate into the inactive slot\n"
            "  boot                 simulate a reboot and report the selected image\n"
            "  confirm              confirm the running trial image\n"
            "options:\n"
            "  --flash PATH         flash file (default: " DEFAULT_FLASH_PATH ")\n"
            "  --fail-at N          simulate power loss at fault point N\n");
}

static char slot_name(uint8_t slot)
{
    return slot == 0u ? 'A' : 'B';
}

static int is_image_error(fwu_status_t st)
{
    return st >= FWU_ERR_IMG_MAGIC && st <= FWU_ERR_IMG_MISMATCH;
}

static void report_error(const char *cmd, fwu_status_t st, const fwu_ctx_t *ctx)
{
    fprintf(stderr, "%s: %s: %s\n", cmd, is_image_error(st) ? "rejected" : "failed",
            fwu_status_str(st));

    const char *hint = NULL;
    uint8_t state = ctx->meta_valid ? ctx->meta.state : 0u;
    switch (st) {
    case FWU_ERR_NO_METADATA:
        hint = "provision the device with 'fwsim init <factory.img>'";
        break;
    case FWU_ERR_STATE:
        if (state == FWU_STATE_TRIAL) {
            hint = "a trial image is running; run 'confirm' to keep it or 'boot' to roll back";
        } else if (state == FWU_STATE_PENDING) {
            hint = "the candidate has not been booted yet; run 'boot' first";
        } else {
            hint = "no trial image is running; 'install' and 'boot' an update first";
        }
        break;
    case FWU_ERR_NO_BOOTABLE:
        hint = "the confirmed image failed verification; see 'fwsim status'";
        break;
    default:
        break;
    }
    if (hint != NULL) {
        fprintf(stderr, "%s: hint: %s\n", cmd, hint);
    }
}

static fwu_status_t stream_file(fwu_ctx_t *ctx, FILE *fp, int *read_error)
{
    uint8_t buf[FILE_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        fwu_status_t st = fwu_install_write(ctx, buf, (uint32_t)n);
        if (st != FWU_OK) {
            return st;
        }
    }
    *read_error = ferror(fp);
    return FWU_OK;
}

static fwu_status_t cmd_install(fwu_ctx_t *ctx, FILE *image, bool factory)
{
    const char *cmd = factory ? "init" : "install";
    int read_error = 0;

    fwu_status_t st = factory ? fwu_factory_begin(ctx) : fwu_install_begin(ctx);
    if (st == FWU_OK) {
        st = stream_file(ctx, image, &read_error);
        if (read_error) {
            fprintf(stderr, "%s: failed: error reading image file\n", cmd);
            return FWU_ERR_ARG;
        }
        fwu_status_t fin = fwu_install_finish(ctx);
        if (st == FWU_OK) {
            st = fin;
        }
    }
    if (st != FWU_OK) {
        if (st != FWU_ERR_POWER_LOSS) {
            report_error(cmd, st, ctx);
        }
        return st;
    }

    const fwu_meta_t *m = &ctx->meta;
    if (factory) {
        printf("init: factory image version 0x%08x in slot %c, CONFIRMED\n",
               (unsigned)m->confirmed_version, slot_name(m->confirmed_slot));
    } else {
        printf("install: version 0x%08x staged in slot %c, PENDING\n",
               (unsigned)m->candidate_version, slot_name(m->candidate_slot));
    }
    return FWU_OK;
}

static fwu_status_t cmd_status(fwu_ctx_t *ctx)
{
    fwu_report_t r;
    fwu_status_t st = fwu_report(ctx, &r);
    if (st != FWU_OK) {
        if (st != FWU_ERR_POWER_LOSS) {
            report_error("status", st, ctx);
        }
        return st;
    }

    if (!r.meta_valid) {
        printf("metadata:  none (unprovisioned; run 'fwsim init <factory.img>')\n");
    } else {
        printf("metadata:  seq=%u sector=META%u state=%s last_event=%s\n",
               (unsigned)r.meta.seq, (unsigned)r.meta_sector, fwu_state_str(r.meta.state),
               fwu_event_str(r.meta.last_event));
        printf("confirmed: slot %c version 0x%08x\n", slot_name(r.meta.confirmed_slot),
               (unsigned)r.meta.confirmed_version);
        if (r.meta.candidate_slot == FWU_SLOT_NONE) {
            printf("candidate: none\n");
        } else {
            printf("candidate: slot %c version 0x%08x\n", slot_name(r.meta.candidate_slot),
                   (unsigned)r.meta.candidate_version);
        }
    }
    for (uint8_t s = 0; s < FLASH_SLOT_COUNT; s++) {
        const char *role = "";
        if (r.meta_valid && s == r.meta.confirmed_slot) {
            role = " [confirmed]";
        } else if (r.meta_valid && s == r.meta.candidate_slot) {
            role = " [candidate]";
        } else if (r.meta_valid) {
            role = " [inactive]";
        }
        if (r.slot_status[s] == FWU_OK) {
            printf("slot %c:    valid   version 0x%08x payload_len=%u crc=0x%08x%s\n", slot_name(s),
                   (unsigned)r.slot_header[s].fw_version, (unsigned)r.slot_header[s].payload_len,
                   (unsigned)r.slot_header[s].payload_crc, role);
        } else {
            printf("slot %c:    invalid (%s)%s\n", slot_name(s), fwu_status_str(r.slot_status[s]),
                   role);
        }
    }
    return FWU_OK;
}

static fwu_status_t cmd_boot(fwu_ctx_t *ctx)
{
    fwu_boot_result_t b;
    fwu_status_t st = fwu_boot(ctx, &b);
    if (st != FWU_OK) {
        if (st != FWU_ERR_POWER_LOSS) {
            report_error("boot", st, ctx);
        }
        return st;
    }
    const char *note = "confirmed";
    if (b.trial) {
        note = "TRIAL, awaiting confirm";
    } else if (b.event == FWU_EVENT_ROLLED_BACK) {
        note = "confirmed; rolled back, trial was not confirmed";
    } else if (b.event == FWU_EVENT_REJECTED) {
        note = "confirmed; pending candidate failed verification";
    }
    printf("boot: slot %c version 0x%08x (%s)\n", slot_name(b.slot), (unsigned)b.fw_version, note);
    return FWU_OK;
}

static fwu_status_t cmd_confirm(fwu_ctx_t *ctx)
{
    fwu_status_t st = fwu_confirm(ctx);
    if (st != FWU_OK) {
        if (st != FWU_ERR_POWER_LOSS) {
            report_error("confirm", st, ctx);
        }
        return st;
    }
    printf("confirm: slot %c version 0x%08x is now confirmed\n",
           slot_name(ctx->meta.confirmed_slot), (unsigned)ctx->meta.confirmed_version);
    return FWU_OK;
}

static int run_command(const char *cmd, const char *arg, const char *flash_path, uint32_t fail_at)
{
    flashsim_t sim;
    flash_dev_t dev;
    fwu_ctx_t ctx;
    FILE *image = NULL;
    bool is_init = strcmp(cmd, "init") == 0;

    if (arg != NULL) {
        image = fopen(arg, "rb");
        if (image == NULL) {
            fprintf(stderr, "%s: cannot open image file '%s'\n", cmd, arg);
            return EXIT_FAIL;
        }
    }

    if (is_init && flashsim_create(flash_path) != FLASH_OK) {
        fprintf(stderr, "init: cannot create flash file '%s'\n", flash_path);
        if (image != NULL) {
            fclose(image);
        }
        return EXIT_FAIL;
    }
    flash_status_t fst = flashsim_open(&sim, flash_path);
    if (fst != FLASH_OK) {
        if (fst == FLASH_ERR_RANGE) {
            fprintf(stderr, "%s: '%s' is not a %u-byte flash file\n", cmd, flash_path,
                    (unsigned)FLASH_TOTAL_SIZE);
        } else {
            fprintf(stderr, "%s: cannot open flash file '%s'\n%s: hint: create it with "
                    "'fwsim --flash %s init <factory.img>'\n", cmd, flash_path, cmd, flash_path);
        }
        if (image != NULL) {
            fclose(image);
        }
        return EXIT_FAIL;
    }
    flashsim_set_fault(&sim, fail_at);
    flashsim_bind(&sim, &dev);

    fwu_status_t st = fwu_open(&ctx, &dev);
    if (st == FWU_OK) {
        if (is_init) {
            st = cmd_install(&ctx, image, true);
        } else if (strcmp(cmd, "install") == 0) {
            st = cmd_install(&ctx, image, false);
        } else if (strcmp(cmd, "status") == 0) {
            st = cmd_status(&ctx);
        } else if (strcmp(cmd, "boot") == 0) {
            st = cmd_boot(&ctx);
        } else {
            st = cmd_confirm(&ctx);
        }
    } else if (st != FWU_ERR_POWER_LOSS) {
        fprintf(stderr, "%s: failed to read metadata: %s\n", cmd, fwu_status_str(st));
    }

    if (fail_at != FLASHSIM_NO_FAULT && st != FWU_ERR_POWER_LOSS) {
        fprintf(stderr, "fwsim: fault points passed: %u\n", (unsigned)sim.fault_points);
    }
    flashsim_close(&sim);
    if (image != NULL) {
        fclose(image);
    }

    if (st == FWU_ERR_POWER_LOSS) {
        fprintf(stderr, "fwsim: simulated power loss at fault point %u\n", (unsigned)fail_at);
        return EXIT_POWER_LOSS;
    }
    return st == FWU_OK ? EXIT_OK : EXIT_FAIL;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (*s == '\0' || *s == '-' || *end != '\0' || v >= FLASHSIM_NO_FAULT) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *flash_path = DEFAULT_FLASH_PATH;
    uint32_t fail_at = FLASHSIM_NO_FAULT;
    int i = 1;

    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc) {
            flash_path = argv[++i];
        } else if (strcmp(argv[i], "--fail-at") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &fail_at) != 0) {
                fprintf(stderr, "fwsim: --fail-at expects a non-negative integer, got '%s'\n",
                        argv[i]);
                return EXIT_USAGE;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return EXIT_OK;
        } else {
            fprintf(stderr, "fwsim: unknown or incomplete option '%s'\n", argv[i]);
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return EXIT_USAGE;
    }

    const char *cmd = argv[i++];
    const char *arg = (i < argc) ? argv[i++] : NULL;
    bool needs_arg = strcmp(cmd, "init") == 0 || strcmp(cmd, "install") == 0;
    bool known = needs_arg || strcmp(cmd, "status") == 0 ||
                 strcmp(cmd, "boot") == 0 || strcmp(cmd, "confirm") == 0;
    if (!known) {
        fprintf(stderr, "fwsim: unknown command '%s'\n", cmd);
        usage(stderr);
        return EXIT_USAGE;
    }
    if (needs_arg != (arg != NULL) || i != argc) {
        fprintf(stderr, "fwsim: '%s' %s\n", cmd,
                needs_arg ? "takes exactly one image file" : "takes no arguments");
        return EXIT_USAGE;
    }

    return run_command(cmd, arg, flash_path, fail_at);
}
