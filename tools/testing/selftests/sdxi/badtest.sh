#!/bin/bash

set -euo pipefail

kmemleak_ctl="/sys/kernel/debug/kmemleak"

kernel_ok_or_fail() {
    local status=ok
    local tainted

    # check taint
    read -r tainted < /proc/sys/kernel/tainted
    case "$tainted" in
        0)
            :
            ;;
        *)
            printf "kernel tainted (%d)\n" "$tainted"
            status=notok
            ;;
    esac

    status="${status:-ok}"
    case "$status" in
        ok)
            :
            ;;
        *)
            printf "failure!\n"
            exit 1
            ;;
    esac
}

sdxi_bind_device() {
    local dev="$1"; shift

    printf "Binding %s to sdxi\n" "$dev"
    printf "%s" "$dev" > /sys/bus/pci/drivers/sdxi/bind
    kernel_ok_or_fail
}

sdxi_unbind_device() {
    local dev="$1"; shift

    printf "Unbinding %s from sdxi\n" "$dev"
    printf "%s" "$dev" > /sys/bus/pci/drivers/sdxi/unbind
    kernel_ok_or_fail
}

dma_debug_setup() {
    local dma_debug_dir="/sys/kernel/debug/dma-api"
    test -d "$dma_debug_dir" || {
        printf "Note: no DMA debug facility found\n"
        return
    }

    # Don't stop reporting on the first error but stop after ten.
    (cd "$dma_debug_dir" && {
         printf "1" > all_errors
         printf "10" > num_errors
     }
    )
}

kmemleak_setup() {
    if test -w "$kmemleak_ctl" ; then
        local count;
        printf "Clearing kmemleak state..."
        printf "scan" > "$kmemleak_ctl"
        count="$(grep -c -e '^unreferenced object' "$kmemleak_ctl" || true)"
        case "$count" in
            0)
                printf "\n"
                ;;
            *)
                discards="$(mktemp --tmpdir kmemleak-discard-XXXX)"
                cat "$kmemleak_ctl" > "$discards"
                printf " %d suspected leaks discarded, saved in %s.\n" \
                       "$count" "$discards"
		printf "clear" > "$kmemleak_ctl"
                ;;
        esac
    else
        printf "Note: kmemleak not available or insufficient privs\n"
    fi
}

kmemleak_report() {
    test -w "$kmemleak_ctl" && {
        local count;
        printf "Performing kmemleak scan... "
        printf "scan" > "$kmemleak_ctl"
        count="$(grep -c -e '^unreferenced object' "$kmemleak_ctl" || true)"
        case "$count" in
            0)
                printf "no leaks found.\n"
                ;;
            *)
                printf "%d leaks found.\n" "$count"
                ;;
        esac
    }
}

libsdxi_sample() {
    local tst="$1"; shift
    if test -x ./samples/"$tst" ; then
        ./samples/"$tst"
        kernel_ok_or_fail
    else
        printf "samples/%s not found, skipping\n" "$tst"
    fi
}

dma_debug_setup
kmemleak_setup

while read -r dev _rest ; do
    sdxi_unbind_device "$dev"
    sdxi_bind_device "$dev"
done < <(lspci -d 0x1022:0x14dc -Dmm)

# If we're in a built libsdxi directory, run the samples
libsdxi_sample "context"
libsdxi_sample "repcopy"
libsdxi_sample "memcopy"
libsdxi_sample "uadd"
libsdxi_sample "write_imm"

kmemleak_report
