#include "TB_syzygy.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <algorithm>
#ifndef _WIN32
#   include <unistd.h>
#   include <sys/mman.h>
#endif

#ifndef _WIN32
#   include <pthread.h>
#   define SEP_CHAR ':'
#   define FD i32
#   define FD_ERR -1

#   define LOCK_T       pthread_mutex_t
#   define LOCK_INIT(x) pthread_mutex_init(&(x), nullptr)
#   define LOCK(x)      pthread_mutex_lock(&(x))
#   define UNLOCK(x)    pthread_mutex_unlock(&(x))
#else
#   ifndef NOMINMAX
#       define NOMINMAX // Disable macros min() and max()
#   endif
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#   undef WIN32_LEAN_AND_MEAN
#   undef NOMINMAX
#   define SEP_CHAR ';'
#   define FD HANDLE
#   define FD_ERR INVALID_HANDLE_VALUE

#   define LOCK_T HANDLE
#   define LOCK_INIT(x) do { x = CreateMutex(nullptr, FALSE, nullptr); } while (false)
#   define LOCK(x) WaitForSingleObject(x, INFINITE)
#   define UNLOCK(x) ReleaseMutex(x)
#endif


#ifndef _MSC_VER
#   define BSWAP32(v) __builtin_bswap32(v)
#   define BSWAP64(v) __builtin_bswap64(v)
#else
#   define BSWAP32(v) _byteswap_ulong(v)
#   define BSWAP64(v) _byteswap_uint64(v)
#endif

#include "BitCount.h"
#include "BitBoard.h"
#include "Position.h"
#include "MoveGenerator.h"
#include "Searcher.h"

namespace TBSyzygy {

    using namespace std;
    using namespace BitBoard;
    using namespace MoveGen;
    using namespace Searcher;

    i32     PieceLimit      = 6;
    Depth   DepthLimit      = 1*DEPTH_ONE;
    bool    UseRule50       = true;

    i32     MaxPieceLimit   = 0;
    u16     Hits            = 0;
    bool    RootInTB        = false;
    Value   ProbeValue      = VALUE_NONE;

    // Core
    namespace {

        // CORE contains engine-independent routines of the tablebase probing code.
        // This should not need to much adaptation to add tablebase probing to
        // a particular engine, provided the engine is written in C or C++.

#define WDLSUFFIX ".rtbw"
#define DTZSUFFIX ".rtbz"
#define WDLDIR "RTBWDIR"
#define DTZDIR "RTBZDIR"

        const u08 WDL_MAGIC[4] ={ 0x71, 0xe8, 0x23, 0x5D };
        const u08 DTZ_MAGIC[4] ={ 0xD7, 0x66, 0x0C, 0xA5 };

#define TBHASHBITS 10

        struct TBHashEntry;

        typedef u64 base_t;

        struct PairsData
        {
            char *table_index;
            u16  *table_size;
            u08   *data;
            u16  *offset;
            u08   *symlen;
            u08   *sympat;
            i32     blocksize;
            i32     idxbits;
            i32     min_len;
            base_t  base[1]; // C++ complains about base[]...
        };

        struct TBEntry
        {
            char *data;
            u64 key;
            u64 mapping;
            u08 ready;
            u08 num;
            bool symmetric;
            bool has_pawns;
        }
#ifndef _WIN32
        __attribute__ ((__may_alias__))
#endif
            ;

        struct TBEntry_piece
        {
            char *data;
            u64 key;
            u64 mapping;
            u08 ready;
            u08 num;
            bool symmetric;
            bool has_pawns;
            u08 enc_type;
            PairsData *precomp[2];
            i32 factor[2][NONE];
            u08 pieces[2][NONE];
            u08 norm[2][NONE];
        };

        struct TBEntry_pawn
        {
            char *data;
            u64 key;
            u64 mapping;
            u08 ready;
            u08 num;
            bool symmetric;
            bool has_pawns;
            u08 pawns[2];
            struct
            {
                PairsData *precomp[2];
                i32 factor[2][NONE];
                u08 pieces[2][NONE];
                u08 norm[2][NONE];
            } file[4];
        };

        struct DTZEntry_piece
        {
            char *data;
            u64 key;
            u64 mapping;
            u08 ready;
            u08 num;
            bool symmetric;
            bool has_pawns;
            u08 enc_type;
            PairsData *precomp;
            i32 factor[NONE];
            u08 pieces[NONE];
            u08 norm[NONE];
            u08 flags; // accurate, mapped, side
            u16 map_idx[4];
            u08 *map;
        };

        struct DTZEntry_pawn
        {
            char *data;
            u64 key;
            u64 mapping;
            u08 ready;
            u08 num;
            bool symmetric;
            bool has_pawns;
            u08 pawns[2];
            struct
            {
                PairsData *precomp;
                i32 factor[NONE];
                u08 pieces[NONE];
                u08 norm[NONE];
            } file[4];
            u08 flags[4];
            u16 map_idx[4][4];
            u08 *map;
        };

        struct TBHashEntry
        {
            u64 key;
            TBEntry *tbe;
        };

        struct DTZTableEntry
        {
            u64 key1;
            u64 key2;
            TBEntry *tbe;
        };

        // ---

#define TBMAX_PIECE 254
#define TBMAX_PAWN 256
#define HSHMAX 5

#define TB_PAWN 1
#define TB_KNIGHT 2
#define TB_BISHOP 3
#define TB_ROOK 4
#define TB_QUEEN 5
#define TB_KING 6

#define TB_WPAWN TB_PAWN
#define TB_BPAWN (TB_PAWN | 8)

        LOCK_T TB_mutex;

        i32 PathCount = 0;
        char **Paths = nullptr;

        u32 TB_piece_count, TB_pawn_count;
        TBEntry_piece TB_piece[TBMAX_PIECE];
        TBEntry_pawn TB_pawn[TBMAX_PAWN];

        TBHashEntry TB_hash[1 << TBHASHBITS][HSHMAX];

#define DTZ_ENTRIES 64

        DTZTableEntry DTZ_table[DTZ_ENTRIES];

        void init_indices ();
        u64 calc_key_from_pcs (u08 *pcs, bool mirror);
        void free_wdl_entry (TBEntry *tbe);
        void free_dtz_entry (TBEntry *tbe);

        FD open_tb (const char *filename, const char *suffix)
        {
            for (i32 i = 0; i < PathCount; ++i)
            {
                char fullname[256];
                strcpy (fullname, Paths[i]);
                strcat (fullname, "/");
                strcat (fullname, filename);
                strcat (fullname, suffix);

                FD fd;
#ifndef _WIN32
                fd = open (fullname, O_RDONLY);
#else
                fd = CreateFile (fullname, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
                if (fd != FD_ERR) return fd;
            }
            return FD_ERR;
        }

        void close_tb (FD fd)
        {
#ifndef _WIN32
            close (fd);
#else
            CloseHandle (fd);
#endif
        }

        char *map_file (const char *filename, const char *suffix, u64 *mapping)
        {
            FD fd = open_tb (filename, suffix);
            if (fd == FD_ERR)
            {
                return nullptr;
            }
#ifndef _WIN32
            stat statbuf;
            fstat (fd, &statbuf);
            *mapping = statbuf.st_size;
            char *data = (char *)mmap (nullptr, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
            if (data == (char *)(-1))
            {
                printf ("Could not mmap() %s.\n", filename);
                exit (1);
            }
#else
            DWORD size_low, size_high;
            size_low = GetFileSize (fd, &size_high);
            //  *size = ((u64)size_high) << 32 | ((u64)size_low);
            HANDLE map = CreateFileMapping (fd, nullptr, PAGE_READONLY, size_high, size_low, nullptr);
            if (map == nullptr)
            {
                printf ("CreateFileMapping() failed.\n");
                exit (1);
            }
            *mapping = (u64)map;
            char *data = (char *)MapViewOfFile (map, FILE_MAP_READ, 0, 0, 0);
            if (data == nullptr)
            {
                printf ("MapViewOfFile() failed, filename = %s%s, error = %lu.\n", filename, suffix, GetLastError ());
                exit (1);
            }
#endif
            close_tb (fd);
            return data;
        }

        void unmap_file (char *data, u64 size)
        {
#ifndef _WIN32
            if (!data) return;
            munmap (data, size);
#else
            if (!data) return;
            UnmapViewOfFile (data);
            CloseHandle ((HANDLE) size);
#endif
        }

        void add_to_hash (TBEntry *tbe, Key key)
        {
            i32 i, hshidx;

            hshidx = key >> (64 - TBHASHBITS);
            i = 0;
            while (i < HSHMAX && TB_hash[hshidx][i].tbe)
                ++i;
            if (i == HSHMAX)
            {
                printf ("HSHMAX too low!\n");
                exit (1);
            }
            else
            {
                TB_hash[hshidx][i].key = key;
                TB_hash[hshidx][i].tbe = tbe;
            }
        }

        char PieceChar[] ={ 'K', 'Q', 'R', 'B', 'N', 'P' };

        void init_tb (const char *filename)
        {
            FD fd = open_tb (filename, WDLSUFFIX);
            if (fd == FD_ERR) return;
            close_tb (fd);

            u08 pcs[16];
            //for (u08 i = 0; i < 16; ++i) pcs[i] = 0x00;
            memset (pcs, 0x00, sizeof (pcs));
            
            u08 color = 0;
            for (auto s = filename; *s; s++)
            {
                switch (*s)
                {
                case 'P':
                    pcs[TB_PAWN | color]++;
                    break;
                case 'N':
                    pcs[TB_KNIGHT | color]++;
                    break;
                case 'B':
                    pcs[TB_BISHOP | color]++;
                    break;
                case 'R':
                    pcs[TB_ROOK | color]++;
                    break;
                case 'Q':
                    pcs[TB_QUEEN | color]++;
                    break;
                case 'K':
                    pcs[TB_KING | color]++;
                    break;
                case 'v':
                    color = 0x08;
                    break;
                }
            }
            for (u08 i = 0; i < 8; ++i)
            {
                if (pcs[i] != pcs[i+8])
                {
                    break;
                }
            }
            Key key1 = calc_key_from_pcs (pcs, false);
            Key key2 = calc_key_from_pcs (pcs, true);
            TBEntry *tbe;
            if (pcs[TB_WPAWN] + pcs[TB_BPAWN] == 0)
            {
                if (TB_piece_count == TBMAX_PIECE)
                {
                    printf ("TBMAX_PIECE limit too low!\n");
                    exit (1);
                }
                tbe = (TBEntry *)&TB_piece[TB_piece_count++];
            }
            else
            {
                if (TB_pawn_count == TBMAX_PAWN)
                {
                    printf ("TBMAX_PAWN limit too low!\n");
                    exit (1);
                }
                tbe = (TBEntry *)&TB_pawn[TB_pawn_count++];
            }
            tbe->key = key1;
            tbe->ready = 0;
            tbe->num = 0;
            for (u08 i = 0; i < 16; ++i)
            {
                tbe->num += pcs[i];
            }
            tbe->symmetric = (key1 == key2);
            tbe->has_pawns = (pcs[TB_WPAWN] + pcs[TB_BPAWN] > 0);
            if (MaxPieceLimit < tbe->num)
            {
                MaxPieceLimit = tbe->num;
            }

            if (tbe->has_pawns)
            {
                auto tbep = (TBEntry_pawn *)tbe;
                tbep->pawns[0] = pcs[TB_WPAWN];
                tbep->pawns[1] = pcs[TB_BPAWN];
                if (   pcs[TB_BPAWN] > 0
                    && (pcs[TB_WPAWN] == 0 || pcs[TB_BPAWN] < pcs[TB_WPAWN])
                   )
                {
                    tbep->pawns[0] = pcs[TB_BPAWN];
                    tbep->pawns[1] = pcs[TB_WPAWN];
                }
            }
            else
            {
                auto tbep = (TBEntry_piece *)tbe;
                u08 i, j;
                for (i = 0, j = 0; i < 16; ++i)
                {
                    if (pcs[i] == 1) ++j;
                }
                if (j >= 3)
                {
                    tbep->enc_type = 0;
                }
                else if (j == 2)
                {
                    tbep->enc_type = 2;
                }
                else
                { /* only for suicide */
                    j = 16;
                    for (i = 0; i < 16; ++i)
                    {
                        if (pcs[i] < j && pcs[i] > 1) j = pcs[i];
                        tbep->enc_type = u08(1 + j);
                    }
                }
            }
            add_to_hash (tbe, key1);
            if (key2 != key1) add_to_hash (tbe, key2);
        }

        const signed char OffDiag[] ={
            0,-1,-1,-1,-1,-1,-1,-1,
            1, 0,-1,-1,-1,-1,-1,-1,
            1, 1, 0,-1,-1,-1,-1,-1,
            1, 1, 1, 0,-1,-1,-1,-1,
            1, 1, 1, 1, 0,-1,-1,-1,
            1, 1, 1, 1, 1, 0,-1,-1,
            1, 1, 1, 1, 1, 1, 0,-1,
            1, 1, 1, 1, 1, 1, 1, 0
        };

        const u08 Triangle[] ={
            6, 0, 1, 2, 2, 1, 0, 6,
            0, 7, 3, 4, 4, 3, 7, 0,
            1, 3, 8, 5, 5, 8, 3, 1,
            2, 4, 5, 9, 9, 5, 4, 2,
            2, 4, 5, 9, 9, 5, 4, 2,
            1, 3, 8, 5, 5, 8, 3, 1,
            0, 7, 3, 4, 4, 3, 7, 0,
            6, 0, 1, 2, 2, 1, 0, 6
        };

        const u08 InvTriangle[] ={
            1, 2, 3, 10, 11, 19, 0, 9, 18, 27
        };

        const u08 InvDiag[] ={
            0, 9, 18, 27, 36, 45, 54, 63,
            7, 14, 21, 28, 35, 42, 49, 56
        };

        const u08 FlipDiag[] ={
            0,  8, 16, 24, 32, 40, 48, 56,
            1,  9, 17, 25, 33, 41, 49, 57,
            2, 10, 18, 26, 34, 42, 50, 58,
            3, 11, 19, 27, 35, 43, 51, 59,
            4, 12, 20, 28, 36, 44, 52, 60,
            5, 13, 21, 29, 37, 45, 53, 61,
            6, 14, 22, 30, 38, 46, 54, 62,
            7, 15, 23, 31, 39, 47, 55, 63
        };

        const u08 Lower[] ={
            28,  0,  1,  2,  3,  4,  5,  6,
            0, 29,  7,  8,  9, 10, 11, 12,
            1,  7, 30, 13, 14, 15, 16, 17,
            2,  8, 13, 31, 18, 19, 20, 21,
            3,  9, 14, 18, 32, 22, 23, 24,
            4, 10, 15, 19, 22, 33, 25, 26,
            5, 11, 16, 20, 23, 25, 34, 27,
            6, 12, 17, 21, 24, 26, 27, 35
        };

        const u08 Diag[] ={
            0,  0,  0,  0,  0,  0,  0,  8,
            0,  1,  0,  0,  0,  0,  9,  0,
            0,  0,  2,  0,  0, 10,  0,  0,
            0,  0,  0,  3, 11,  0,  0,  0,
            0,  0,  0, 12,  4,  0,  0,  0,
            0,  0, 13,  0,  0,  5,  0,  0,
            0, 14,  0,  0,  0,  0,  6,  0,
            15,  0,  0,  0,  0,  0,  0,  7
        };

        const u08 Flap[] ={
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 6, 12, 18, 18, 12, 6, 0,
            1, 7, 13, 19, 19, 13, 7, 1,
            2, 8, 14, 20, 20, 14, 8, 2,
            3, 9, 15, 21, 21, 15, 9, 3,
            4, 10, 16, 22, 22, 16, 10, 4,
            5, 11, 17, 23, 23, 17, 11, 5,
            0, 0, 0, 0, 0, 0, 0, 0
        };

        const u08 InvFlap[] ={
            8, 16, 24, 32, 40, 48,
            9, 17, 25, 33, 41, 49,
            10, 18, 26, 34, 42, 50,
            11, 19, 27, 35, 43, 51
        };

        const u08 Ptwist[] =
        {
            0, 0, 0, 0, 0, 0, 0, 0,
            47, 35, 23, 11, 10, 22, 34, 46,
            45, 33, 21, 9, 8, 20, 32, 44,
            43, 31, 19, 7, 6, 18, 30, 42,
            41, 29, 17, 5, 4, 16, 28, 40,
            39, 27, 15, 3, 2, 14, 26, 38,
            37, 25, 13, 1, 0, 12, 24, 36,
            0, 0, 0, 0, 0, 0, 0, 0
        };
        const u08 InvPtwist[] ={
            52, 51, 44, 43, 36, 35, 28, 27, 20, 19, 12, 11,
            53, 50, 45, 42, 37, 34, 29, 26, 21, 18, 13, 10,
            54, 49, 46, 41, 38, 33, 30, 25, 22, 17, 14, 9,
            55, 48, 47, 40, 39, 32, 31, 24, 23, 16, 15, 8
        };

        const u08 FileToFile[] ={
            0, 1, 2, 3, 3, 2, 1, 0
        };

        const short KK_idx[10][64] ={
            { -1, -1, -1,  0,  1,  2,  3,  4,
            -1, -1, -1,  5,  6,  7,  8,  9,
            10, 11, 12, 13, 14, 15, 16, 17,
            18, 19, 20, 21, 22, 23, 24, 25,
            26, 27, 28, 29, 30, 31, 32, 33,
            34, 35, 36, 37, 38, 39, 40, 41,
            42, 43, 44, 45, 46, 47, 48, 49,
            50, 51, 52, 53, 54, 55, 56, 57 },
            { 58, -1, -1, -1, 59, 60, 61, 62,
            63, -1, -1, -1, 64, 65, 66, 67,
            68, 69, 70, 71, 72, 73, 74, 75,
            76, 77, 78, 79, 80, 81, 82, 83,
            84, 85, 86, 87, 88, 89, 90, 91,
            92, 93, 94, 95, 96, 97, 98, 99,
            100,101,102,103,104,105,106,107,
            108,109,110,111,112,113,114,115 },
            { 116,117, -1, -1, -1,118,119,120,
            121,122, -1, -1, -1,123,124,125,
            126,127,128,129,130,131,132,133,
            134,135,136,137,138,139,140,141,
            142,143,144,145,146,147,148,149,
            150,151,152,153,154,155,156,157,
            158,159,160,161,162,163,164,165,
            166,167,168,169,170,171,172,173 },
            { 174, -1, -1, -1,175,176,177,178,
            179, -1, -1, -1,180,181,182,183,
            184, -1, -1, -1,185,186,187,188,
            189,190,191,192,193,194,195,196,
            197,198,199,200,201,202,203,204,
            205,206,207,208,209,210,211,212,
            213,214,215,216,217,218,219,220,
            221,222,223,224,225,226,227,228 },
            { 229,230, -1, -1, -1,231,232,233,
            234,235, -1, -1, -1,236,237,238,
            239,240, -1, -1, -1,241,242,243,
            244,245,246,247,248,249,250,251,
            252,253,254,255,256,257,258,259,
            260,261,262,263,264,265,266,267,
            268,269,270,271,272,273,274,275,
            276,277,278,279,280,281,282,283 },
            { 284,285,286,287,288,289,290,291,
            292,293, -1, -1, -1,294,295,296,
            297,298, -1, -1, -1,299,300,301,
            302,303, -1, -1, -1,304,305,306,
            307,308,309,310,311,312,313,314,
            315,316,317,318,319,320,321,322,
            323,324,325,326,327,328,329,330,
            331,332,333,334,335,336,337,338 },
            { -1, -1,339,340,341,342,343,344,
            -1, -1,345,346,347,348,349,350,
            -1, -1,441,351,352,353,354,355,
            -1, -1, -1,442,356,357,358,359,
            -1, -1, -1, -1,443,360,361,362,
            -1, -1, -1, -1, -1,444,363,364,
            -1, -1, -1, -1, -1, -1,445,365,
            -1, -1, -1, -1, -1, -1, -1,446 },
            { -1, -1, -1,366,367,368,369,370,
            -1, -1, -1,371,372,373,374,375,
            -1, -1, -1,376,377,378,379,380,
            -1, -1, -1,447,381,382,383,384,
            -1, -1, -1, -1,448,385,386,387,
            -1, -1, -1, -1, -1,449,388,389,
            -1, -1, -1, -1, -1, -1,450,390,
            -1, -1, -1, -1, -1, -1, -1,451 },
            { 452,391,392,393,394,395,396,397,
            -1, -1, -1, -1,398,399,400,401,
            -1, -1, -1, -1,402,403,404,405,
            -1, -1, -1, -1,406,407,408,409,
            -1, -1, -1, -1,453,410,411,412,
            -1, -1, -1, -1, -1,454,413,414,
            -1, -1, -1, -1, -1, -1,455,415,
            -1, -1, -1, -1, -1, -1, -1,456 },
            { 457,416,417,418,419,420,421,422,
            -1,458,423,424,425,426,427,428,
            -1, -1, -1, -1, -1,429,430,431,
            -1, -1, -1, -1, -1,432,433,434,
            -1, -1, -1, -1, -1,435,436,437,
            -1, -1, -1, -1, -1,459,438,439,
            -1, -1, -1, -1, -1, -1,460,440,
            -1, -1, -1, -1, -1, -1, -1,461 }
        };

        i32 Binomial[5][64];
        i32 PawnIdx[5][24];
        i32 PFactor[5][4];

        void init_indices ()
        {
            i32 i, j, k;

            // Binomial[k-1][n] = Bin(n, k)
            for (i = 0; i < 5; ++i)
            {
                for (j = 0; j < 64; ++j)
                {
                    i32 f = j;
                    i32 l = 1;
                    for (k = 1; k <= i; ++k)
                    {
                        f *= (j - k);
                        l *= (k + 1);
                    }
                    Binomial[i][j] = f / l;
                }
            }
            for (i = 0; i < 5; ++i)
            {
                i32 s = 0;
                for (j = 0; j < 6; ++j)
                {
                    PawnIdx[i][j] = s;
                    s += (i == 0) ? 1 : Binomial[i - 1][Ptwist[InvFlap[j]]];
                }
                PFactor[i][0] = s;
                s = 0;
                for (; j < 12; ++j)
                {
                    PawnIdx[i][j] = s;
                    s += (i == 0) ? 1 : Binomial[i - 1][Ptwist[InvFlap[j]]];
                }
                PFactor[i][1] = s;
                s = 0;
                for (; j < 18; ++j)
                {
                    PawnIdx[i][j] = s;
                    s += (i == 0) ? 1 : Binomial[i - 1][Ptwist[InvFlap[j]]];
                }
                PFactor[i][2] = s;
                s = 0;
                for (; j < 24; ++j)
                {
                    PawnIdx[i][j] = s;
                    s += (i == 0) ? 1 : Binomial[i - 1][Ptwist[InvFlap[j]]];
                }
                PFactor[i][3] = s;
            }
        }

        u64 encode_piece (TBEntry_piece *ptr, u08 *norm, i32 *pos, i32 *factor)
        {
            u64 idx;

            u08 n = ptr->num;

            if (pos[0] & 0x04)
            {
                for (u08 i = 0; i < n; ++i)
                {
                    pos[i] ^= 0x07;
                }
            }
            if (pos[0] & 0x20)
            {
                for (u08 i = 0; i < n; ++i)
                {
                    pos[i] ^= 0x38;
                }
            }

            u08 i;
            for (i = 0; i < n; ++i)
            {
                if (OffDiag[pos[i]]) break;
            }
            if (i < (ptr->enc_type == 0 ? 3 : 2) && OffDiag[pos[i]] > 0)
            {
                for (i = 0; i < n; ++i)
                {
                    pos[i] = FlipDiag[pos[i]];
                }
            }

            i32 j;
            switch (ptr->enc_type)
            {

            case 0: /* 111 */
                i = (pos[1] > pos[0]);
                j = (pos[2] > pos[0]) + (pos[2] > pos[1]);

                if (OffDiag[pos[0]])
                    idx = Triangle[pos[0]] * 63*62 + (pos[1] - i) * 62 + (pos[2] - j);
                else if (OffDiag[pos[1]])
                    idx = 6*63*62 + Diag[pos[0]] * 28*62 + Lower[pos[1]] * 62 + pos[2] - j;
                else if (OffDiag[pos[2]])
                    idx = 6*63*62 + 4*28*62 + (Diag[pos[0]]) * 7*28 + (Diag[pos[1]] - i) * 28 + Lower[pos[2]];
                else
                    idx = 6*63*62 + 4*28*62 + 4*7*28 + (Diag[pos[0]] * 7*6) + (Diag[pos[1]] - i) * 6 + (Diag[pos[2]] - j);
                i = 3;
                break;

            case 1: /* K3 */
                j = (pos[2] > pos[0]) + (pos[2] > pos[1]);

                idx = KK_idx[Triangle[pos[0]]][pos[1]];
                if (idx < 441)
                {
                    idx = idx + 441 * (pos[2] - j);
                }
                else
                {
                    idx = 441*62 + (idx - 441) + 21 * Lower[pos[2]];
                    if (!OffDiag[pos[2]])
                        idx -= j * 21;
                }
                i = 3;
                break;

            default: /* K2 */
                idx = KK_idx[Triangle[pos[0]]][pos[1]];
                i = 2;
                break;
            }
            idx *= factor[0];

            for (; i < n;)
            {
                i32 t = norm[i];
                for (j = i; j < i + t; ++j)
                {
                    for (i32 k = j + 1; k < i + t; ++k)
                    {
                        if (pos[j] > pos[k]) swap (pos[j], pos[k]);
                    }
                }
                i32 s = 0;
                for (u08 m = i; m < i + t; m++)
                {
                    i32 p = pos[m], l;
                    for (l = 0, j = 0; l < i; ++l)
                    {
                        j += (p > pos[l]);
                    }
                    s += Binomial[m - i][p - j];
                }
                idx += ((u64)s) * ((u64)factor[i]);
                i += t;
            }

            return idx;
        }

        // determine file of leftmost pawn and sort pawns
        i32 pawn_file (TBEntry_pawn *ptr, i32 *pos)
        {
            i32 i;

            for (i = 1; i < ptr->pawns[0]; ++i)
                if (Flap[pos[0]] > Flap[pos[i]])
                    swap (pos[0], pos[i]);

            return FileToFile[pos[0] & 0x07];
        }

        u64 encode_pawn (TBEntry_pawn *ptr, u08 *norm, i32 *pos, i32 *factor)
        {
            u64 idx;
            i32 i, j, k, m, s, t;
            i32 n = ptr->num;

            if (pos[0] & 0x04)
                for (i = 0; i < n; ++i)
                    pos[i] ^= 0x07;

            for (i = 1; i < ptr->pawns[0]; ++i)
                for (j = i + 1; j < ptr->pawns[0]; ++j)
                    if (Ptwist[pos[i]] < Ptwist[pos[j]])
                        swap (pos[i], pos[j]);

            t = ptr->pawns[0] - 1;
            idx = PawnIdx[t][Flap[pos[0]]];
            for (i = t; i > 0; --i)
                idx += Binomial[t - i][Ptwist[pos[i]]];
            idx *= factor[0];

            // remaining pawns
            i = ptr->pawns[0];
            t = i + ptr->pawns[1];
            if (t > i)
            {
                for (j = i; j < t; ++j)
                {
                    for (k = j + 1; k < t; ++k)
                    {
                        if (pos[j] > pos[k]) swap (pos[j], pos[k]);
                    }
                }
                s = 0;
                for (m = i; m < t; m++)
                {
                    i32 p = pos[m];
                    for (k = 0, j = 0; k < i; ++k)
                    {
                        j += (p > pos[k]);
                    }
                    s += Binomial[m - i][p - j - 8];
                }
                idx += ((u64)s) * ((u64)factor[i]);
                i = t;
            }

            for (; i < n;)
            {
                t = norm[i];
                for (j = i; j < i + t; ++j)
                    for (k = j + 1; k < i + t; ++k)
                        if (pos[j] > pos[k]) swap (pos[j], pos[k]);
                s = 0;
                for (m = i; m < i + t; m++)
                {
                    i32 p = pos[m];
                    for (k = 0, j = 0; k < i; ++k)
                        j += (p > pos[k]);
                    s += Binomial[m - i][p - j];
                }
                idx += ((u64)s) * ((u64)factor[i]);
                i += t;
            }

            return idx;
        }

        // place k like pieces on n squares
        i32 subfactor (i32 k, i32 n)
        {
            i32 i, f, l;

            f = n;
            l = 1;
            for (i = 1; i < k; ++i)
            {
                f *= n - i;
                l *= i + 1;
            }

            return f / l;
        }

        u64 calc_factors_piece (i32 *factor, i32 num, i32 order, u08 *norm, u08 enc_type)
        {
            static i32 pivfac[] ={ 31332, 28056, 462 };

            i32 n = 64 - norm[0];
            u64 f = 1;
            for (i32 i = norm[0], k = 0; i < num || k == order; ++k)
            {
                if (k == order)
                {
                    factor[0] = static_cast<i32>(f);
                    f *= pivfac[enc_type];
                }
                else
                {
                    factor[i] = static_cast<i32>(f);
                    f *= subfactor (norm[i], n);
                    n -= norm[i];
                    i += norm[i];
                }
            }
            return f;
        }

        u64 calc_factors_pawn (i32 *factor, i32 num, i32 order1, i32 order2, u08 *norm, i32 file)
        {
            i32 i = norm[0];
            if (order2 < 0x0F) i += norm[i];
            i32 n = 64 - i;

            u64 f = 1;
            for (i32 k = 0; i < num || k == order1 || k == order2; ++k)
            {
                if (k == order1)
                {
                    factor[0] = static_cast<i32>(f);
                    f *= PFactor[norm[0] - 1][file];
                }
                else if (k == order2)
                {
                    factor[norm[0]] = static_cast<i32>(f);
                    f *= subfactor (norm[norm[0]], 48 - norm[0]);
                }
                else
                {
                    factor[i] = static_cast<i32>(f);
                    f *= subfactor (norm[i], n);
                    n -= norm[i];
                    i += norm[i];
                }
            }
            return f;
        }

        void set_norm_piece (TBEntry_piece *ptr, u08 *norm, u08 *pieces)
        {
            for (u08 i = 0; i < ptr->num; ++i)
            {
                norm[i] = 0;
            }
            switch (ptr->enc_type)
            {
            case 0:
                norm[0] = 3;
                break;
            case 2:
                norm[0] = 2;
                break;
            default:
                norm[0] = u08(ptr->enc_type - 1);
                break;
            }
            for (u08 i = norm[0]; i < ptr->num; i += norm[i])
            {
                for (u08 j = i; j < ptr->num && pieces[j] == pieces[i]; ++j)
                {
                    norm[i]++;
                }
            }
        }

        void set_norm_pawn (TBEntry_pawn *ptr, u08 *norm, u08 *pieces)
        {
            for (u08 i = 0; i < ptr->num; ++i)
            {
                norm[i] = 0;
            }
            norm[0] = ptr->pawns[0];
            if (ptr->pawns[1] != 0)
            {
                norm[ptr->pawns[0]] = ptr->pawns[1];
            }
            for (u08 i = ptr->pawns[0] + ptr->pawns[1]; i < ptr->num; i += norm[i])
            {
                for (u08 j = i; j < ptr->num && pieces[j] == pieces[i]; ++j)
                {
                    norm[i]++;
                }
            }
        }

        void setup_pieces_piece (TBEntry_piece *ptr, u08 *data, u64 *tb_size)
        {
            i32 order;
            for (u08 i = 0; i < ptr->num; ++i)
            {
                ptr->pieces[0][i] = u08(data[i + 1] & 0x0F);
            }
            order = data[0] & 0x0F;
            set_norm_piece (ptr, ptr->norm[0], ptr->pieces[0]);
            tb_size[0] = calc_factors_piece (ptr->factor[0], ptr->num, order, ptr->norm[0], ptr->enc_type);

            for (u08 i = 0; i < ptr->num; ++i)
            {
                ptr->pieces[1][i] = u08(data[i + 1] >> 4);
            }
            order = data[0] >> 4;
            set_norm_piece (ptr, ptr->norm[1], ptr->pieces[1]);
            tb_size[1] = calc_factors_piece (ptr->factor[1], ptr->num, order, ptr->norm[1], ptr->enc_type);
        }

        void setup_pieces_piece_dtz (DTZEntry_piece *ptr, u08 *data, u64 *tb_size)
        {
            i32 order;
            for (u08 i = 0; i < ptr->num; ++i)
            {
                ptr->pieces[i] = u08(data[i + 1] & 0x0F);
            }
            order = data[0] & 0x0F;
            set_norm_piece ((TBEntry_piece *)ptr, ptr->norm, ptr->pieces);
            tb_size[0] = calc_factors_piece (ptr->factor, ptr->num, order, ptr->norm, ptr->enc_type);
        }

        void setup_pieces_pawn (TBEntry_pawn *ptr, u08 *data, u64 *tb_size, i32 f)
        {
            i32 order1, order2;

            u08 j = 1 + (ptr->pawns[1] > 0);
            order1 = data[0] & 0x0F;
            order2 = ptr->pawns[1] ? (data[1] & 0x0F) : 0x0F;
            for (u08 i = 0; i < ptr->num; ++i)
            {
                ptr->file[f].pieces[0][i] = u08(data[i + j] & 0x0F);
            }
            set_norm_pawn (ptr, ptr->file[f].norm[0], ptr->file[f].pieces[0]);
            tb_size[0] = calc_factors_pawn (ptr->file[f].factor[0], ptr->num, order1, order2, ptr->file[f].norm[0], f);

            order1 = data[0] >> 4;
            order2 = ptr->pawns[1] ? (data[1] >> 4) : 0x0F;
            for (u08 i = 0; i < ptr->num; ++i)
            {
                ptr->file[f].pieces[1][i] = u08(data[i + j] >> 4);
            }
            set_norm_pawn (ptr, ptr->file[f].norm[1], ptr->file[f].pieces[1]);
            tb_size[1] = calc_factors_pawn (ptr->file[f].factor[1], ptr->num, order1, order2, ptr->file[f].norm[1], f);
        }

        void setup_pieces_pawn_dtz (DTZEntry_pawn *ptr, u08 *data, u64 *tb_size, i32 f)
        {
            i32 order1, order2;

            u08 j = 1 + (ptr->pawns[1] > 0);
            order1 = data[0] & 0x0F;
            order2 = ptr->pawns[1] ? (data[1] & 0x0F) : 0x0F;
            for (u08 i = 0; i < ptr->num; ++i)
            {
                ptr->file[f].pieces[i] = u08(data[i + j] & 0x0F);
            }
            set_norm_pawn ((TBEntry_pawn *)ptr, ptr->file[f].norm, ptr->file[f].pieces);
            tb_size[0] = calc_factors_pawn (ptr->file[f].factor, ptr->num, order1, order2, ptr->file[f].norm, f);
        }

        void calc_symlen (PairsData *d, i32 s, char *tmp)
        {
            i32 s1, s2;

            u08* w = d->sympat + 3 * s;
            s2 = (w[2] << 4) | (w[1] >> 4);
            if (s2 == 0x0fff)
            {
                d->symlen[s] = 0;
            }
            else
            {
                s1 = ((w[1] & 0x0F) << 8) | w[0];
                if (!tmp[s1]) calc_symlen (d, s1, tmp);
                if (!tmp[s2]) calc_symlen (d, s2, tmp);
                d->symlen[s] = u08(d->symlen[s1] + d->symlen[s2] + 1);
            }
            tmp[s] = 1;
        }

        u16 read_u16 (u08* d)
        {
            return u16(d[0] | (d[1] << 8));
        }

        u32 read_u32 (u08* d)
        {
            return u32(d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24));
        }

        PairsData *setup_pairs (u08 *data, u64 tb_size, u64 *size, u08 **next, u08 *flags, bool wdl)
        {
            PairsData *d;
            *flags = data[0];
            if (data[0] & 0x80)
            {
                d = (PairsData *)malloc (sizeof (PairsData));
                d->idxbits = 0;
                if (wdl)
                {
                    d->min_len = data[1];
                }
                else
                {
                    d->min_len = 0;
                }
                *next = data + 2;
                size[0] = size[1] = size[2] = 0;
                return d;
            }

            i32 blocksize = data[1];
            i32 idxbits = data[2];
            i32 real_num_blocks = read_u32 (&data[4]);
            i32 num_blocks = real_num_blocks + *(u08 *)(&data[3]);
            i32 max_len = data[8];
            i32 min_len = data[9];
            i32 h = max_len - min_len + 1;
            i32 num_syms = read_u16 (&data[10 + 2 * h]);
            d = (PairsData *)malloc (sizeof (PairsData) + (h - 1) * sizeof (base_t) + num_syms);
            d->blocksize = blocksize;
            d->idxbits = idxbits;
            d->offset = (u16*)(&data[10]);
            d->symlen = ((u08 *)d) + sizeof (PairsData) + (h - 1) * sizeof (base_t);
            d->sympat = &data[12 + 2 * h];
            d->min_len = min_len;
            *next = &data[12 + 2 * h + 3 * num_syms + (num_syms & 1)];

            u64 num_indices = (tb_size + (1ULL << idxbits) - 1) >> idxbits;
            size[0] = 6ULL * num_indices;
            size[1] = 2ULL * num_blocks;
            size[2] = (1ULL << blocksize) * real_num_blocks;

            // char tmp[num_syms];
            char tmp[4096];
            for (i32 i = 0; i < num_syms; ++i)
            {
                tmp[i] = 0;
            }
            for (i32 i = 0; i < num_syms; ++i)
            {
                if (tmp[i] == 0)
                {
                    calc_symlen (d, i, tmp);
                }
            }
            d->base[h - 1] = 0;
            for (i32 i = h - 2; i >= 0; --i)
            {
                d->base[i] = (d->base[i + 1] + read_u16 ((u08*)(d->offset + i)) - read_u16 ((u08*)(d->offset + i + 1))) / 2;
            }
            for (i32 i = 0; i < h; ++i)
            {
                d->base[i] <<= 64 - (min_len + i);
            }
            d->offset -= d->min_len;

            return d;
        }

        bool init_table_wdl (TBEntry *entry, char *str)
        {
            u08 *next;
            u64 tb_size[8];
            u64 size[8 * 3];
            u08 flags;

            // first mmap the table into memory

            entry->data = map_file (str, WDLSUFFIX, &entry->mapping);
            if (!entry->data)
            {
                printf ("Could not find %s" WDLSUFFIX, str);
                return false;
            }

            u08 *data = (u08 *)entry->data;
            if (   data[0] != WDL_MAGIC[0]
                || data[1] != WDL_MAGIC[1]
                || data[2] != WDL_MAGIC[2]
                || data[3] != WDL_MAGIC[3]
               )
            {
                printf ("Corrupted table.\n");
                unmap_file (entry->data, entry->mapping);
                entry->data = 0;
                return false;
            }

            u08 split = data[4] & 0x01;
            u08 files = data[4] & 0x02 ? 4 : 1;

            data += 5;

            if (entry->has_pawns)
            {
                auto tbep = (TBEntry_pawn *)entry;
                u08 s = 1 + (tbep->pawns[1] > 0);
                u08 f;
                for (f = 0; f < 4; ++f)
                {
                    setup_pieces_pawn ((TBEntry_pawn *)tbep, data, &tb_size[2 * f], f);
                    data += tbep->num + s;
                }
                data += ((uintptr_t)data) & 0x01;

                for (f = 0; f < files; ++f)
                {
                    tbep->file[f].precomp[0] = setup_pairs (data, tb_size[2 * f], &size[6 * f], &next, &flags, true);
                    data = next;
                    if (split)
                    {
                        tbep->file[f].precomp[1] = setup_pairs (data, tb_size[2 * f + 1], &size[6 * f + 3], &next, &flags, true);
                        data = next;
                    }
                    else
                    {
                        tbep->file[f].precomp[1] = nullptr;
                    }
                }

                for (f = 0; f < files; ++f)
                {
                    tbep->file[f].precomp[0]->table_index = (char *)data;
                    data += size[6 * f];
                    if (split)
                    {
                        tbep->file[f].precomp[1]->table_index = (char *)data;
                        data += size[6 * f + 3];
                    }
                }

                for (f = 0; f < files; ++f)
                {
                    tbep->file[f].precomp[0]->table_size = (u16 *)data;
                    data += size[6 * f + 1];
                    if (split)
                    {
                        tbep->file[f].precomp[1]->table_size = (u16 *)data;
                        data += size[6 * f + 4];
                    }
                }

                for (f = 0; f < files; ++f)
                {
                    data = (u08 *)((((uintptr_t)data) + 0x3F) & ~0x3F);
                    tbep->file[f].precomp[0]->data = data;
                    data += size[6 * f + 2];
                    if (split)
                    {
                        data = (u08 *)((((uintptr_t)data) + 0x3F) & ~0x3F);
                        tbep->file[f].precomp[1]->data = data;
                        data += size[6 * f + 5];
                    }
                }
            }
            else
            {
                TBEntry_piece *tbep = (TBEntry_piece *)entry;
                setup_pieces_piece (tbep, data, &tb_size[0]);
                data += tbep->num + 1;
                data += ((uintptr_t)data) & 0x01;

                tbep->precomp[0] = setup_pairs (data, tb_size[0], &size[0], &next, &flags, true);
                data = next;
                if (split)
                {
                    tbep->precomp[1] = setup_pairs (data, tb_size[1], &size[3], &next, &flags, true);
                    data = next;
                }
                else
                {
                    tbep->precomp[1] = nullptr;
                }
                tbep->precomp[0]->table_index = (char *)data;
                data += size[0];
                if (split)
                {
                    tbep->precomp[1]->table_index = (char *)data;
                    data += size[3];
                }

                tbep->precomp[0]->table_size = (u16 *)data;
                data += size[1];
                if (split)
                {
                    tbep->precomp[1]->table_size = (u16 *)data;
                    data += size[4];
                }

                data = (u08 *)((((uintptr_t)data) + 0x3F) & ~0x3F);
                tbep->precomp[0]->data = data;
                data += size[2];
                if (split)
                {
                    data = (u08 *)((((uintptr_t)data) + 0x3F) & ~0x3F);
                    tbep->precomp[1]->data = data;
                }
            }
            return true;
        }

        bool init_table_dtz (TBEntry *entry)
        {
            u08 *data = (u08 *)entry->data;
            if (data == nullptr)
            {
                return false;
            }

            if (   data[0] != DTZ_MAGIC[0]
                || data[1] != DTZ_MAGIC[1]
                || data[2] != DTZ_MAGIC[2]
                || data[3] != DTZ_MAGIC[3]
               )
            {
                printf ("Corrupted table.\n");
                return false;
            }

            u64 tb_size[4];
            u64 size[4 * 3];

            u08 files = data[4] & 0x02 ? 4 : 1;

            data += 5;

            if (entry->has_pawns)
            {
                auto tbep = (DTZEntry_pawn *)entry;
                u08 s = 1 + (tbep->pawns[1] > 0);
                u08 f;
                for (f = 0; f < 4; ++f)
                {
                    setup_pieces_pawn_dtz (tbep, data, &tb_size[f], f);
                    data += tbep->num + s;
                }
                data += ((uintptr_t)data) & 0x01;
                
                u08 *next;
                for (f = 0; f < files; ++f)
                {
                    tbep->file[f].precomp = setup_pairs (data, tb_size[f], &size[3 * f], &next, &(tbep->flags[f]), false);
                    data = next;
                }

                tbep->map = data;
                for (f = 0; f < files; ++f)
                {
                    if (tbep->flags[f] & 2)
                    {
                        i32 i;
                        for (i = 0; i < 4; ++i)
                        {
                            tbep->map_idx[f][i] = static_cast<u16>(data + 1 - tbep->map);
                            data += 1 + data[0];
                        }
                    }
                }
                data += ((uintptr_t)data) & 0x01;

                for (f = 0; f < files; ++f)
                {
                    tbep->file[f].precomp->table_index = (char *)data;
                    data += size[3 * f];
                }

                for (f = 0; f < files; ++f)
                {
                    tbep->file[f].precomp->table_size = (u16 *)data;
                    data += size[3 * f + 1];
                }

                for (f = 0; f < files; ++f)
                {
                    data = (u08 *)((((uintptr_t)data) + 0x3F) & ~0x3F);
                    tbep->file[f].precomp->data = data;
                    data += size[3 * f + 2];
                }
            }
            else
            {
                auto ptr = (DTZEntry_piece *)entry;
                setup_pieces_piece_dtz (ptr, data, &tb_size[0]);
                data += ptr->num + 1;
                data += ((uintptr_t)data) & 0x01;

                u08 *next;
                ptr->precomp = setup_pairs (data, tb_size[0], &size[0], &next, &(ptr->flags), false);
                data = next;

                ptr->map = data;
                if (ptr->flags & 2)
                {
                    i32 i;
                    for (i = 0; i < 4; ++i)
                    {
                        ptr->map_idx[i] = static_cast<u16>(data + 1 - ptr->map);
                        data += 1 + data[0];
                    }
                    data += ((uintptr_t)data) & 0x01;
                }

                ptr->precomp->table_index = (char *)data;
                data += size[0];

                ptr->precomp->table_size = (u16 *)data;
                data += size[1];

                data = (u08 *)((((uintptr_t)data) + 0x3F) & ~0x3F);
                ptr->precomp->data = data;
                data += size[2];
            }
            return true;
        }

        template<bool LittleEndian>
        u08 decompress_pairs (PairsData *d, u64 idx)
        {
            if (!d->idxbits)
            {
                return u08(d->min_len);
            }
            u32 mainidx = static_cast<u32>(idx >> d->idxbits);
            i32 litidx = (idx & ((1ULL << d->idxbits) - 1)) - (1ULL << (d->idxbits - 1));
            u32 block = *(u32 *)(d->table_index + 6 * mainidx);
            if (!LittleEndian)
            {
                block = BSWAP32 (block);
            }
            u16 idxOffset = *(u16 *)(d->table_index + 6 * mainidx + 4);
            if (!LittleEndian)
            {
                idxOffset = u16 ((idxOffset << 8) | (idxOffset >> 8));
            }
            litidx += idxOffset;

            if (litidx < 0)
            {
                do
                {
                    litidx += d->table_size[--block] + 1;
                } while (litidx < 0);
            }
            else
            {
                while (litidx > d->table_size[block])
                    litidx -= d->table_size[block++] + 1;
            }

            u32 *ptr = (u32 *)(d->data + (block << d->blocksize));

            i32 m = d->min_len;
            u16 *offset = d->offset;
            base_t *base = d->base - m;
            u08 *symlen = d->symlen;
            i32 sym, bitcnt;

            u64 code = *((u64 *)ptr);
            if (LittleEndian)
            {
                code = BSWAP64 (code);
            }
            ptr += 2;
            bitcnt = 0; // number of "empty bits" in code
            while (true)
            {
                i32 l = m;
                while (code < base[l]) ++l;
                sym = offset[l];
                if (!LittleEndian)
                {
                    sym = ((sym & 0xff) << 8) | (sym >> 8);
                }
                sym += static_cast<i32>((code - base[l]) >> (64 - l));
                if (litidx < (i32)symlen[sym] + 1) break;
                litidx -= (i32)symlen[sym] + 1;
                code <<= l;
                bitcnt += l;
                if (bitcnt >= 32)
                {
                    bitcnt -= 32;
                    u32 tmp = *ptr++;
                    if (LittleEndian)
                    {
                        tmp = BSWAP32 (tmp);
                    }
                    code |= ((u64)tmp) << bitcnt;
                }
            }

            u08 *sympat = d->sympat;
            while (symlen[sym] != 0)
            {
                u08* w = sympat + (3 * sym);
                i32 s1 = ((w[1] & 0xf) << 8) | w[0];
                if (litidx < (i32)symlen[s1] + 1)
                {
                    sym = s1;
                }
                else
                {
                    litidx -= (i32)symlen[s1] + 1;
                    sym = (w[2] << 4) | (w[1] >> 4);
                }
            }

            return sympat[3 * sym];
        }

        void load_dtz_table (char *str, u64 key1, u64 key2)
        {
            DTZ_table[0].key1 = key1;
            DTZ_table[0].key2 = key2;
            DTZ_table[0].tbe  = nullptr;

            // find corresponding WDL entry
            auto tbhe = TB_hash[key1 >> (64 - TBHASHBITS)];
            u08 i;
            for (i = 0; i < HSHMAX; ++i)
            {
                if (tbhe[i].key == key1) break;
            }
            if (i == HSHMAX) return;
            auto tbe = tbhe[i].tbe;

            auto ptbe = (TBEntry *)malloc (tbe->has_pawns ?
                sizeof (DTZEntry_pawn) :
                sizeof (DTZEntry_piece));

            ptbe->data = map_file (str, DTZSUFFIX, &ptbe->mapping);
            ptbe->key = tbe->key;
            ptbe->num = tbe->num;
            ptbe->symmetric = tbe->symmetric;
            ptbe->has_pawns = tbe->has_pawns;
            if (ptbe->has_pawns)
            {
                auto dtzep = (DTZEntry_pawn *)ptbe;
                dtzep->pawns[0] = ((TBEntry_pawn *)tbe)->pawns[0];
                dtzep->pawns[1] = ((TBEntry_pawn *)tbe)->pawns[1];
            }
            else
            {
                auto dtzep = (DTZEntry_piece *)ptbe;
                dtzep->enc_type = ((TBEntry_piece *)tbe)->enc_type;
            }
            if (!init_table_dtz (ptbe))
            {
                free (ptbe);
            }
            else
            {
                DTZ_table[0].tbe = ptbe;
            }
        }

        void free_wdl_entry (TBEntry *tbe)
        {
            unmap_file (tbe->data, tbe->mapping);
            if (tbe->has_pawns)
            {
                auto tbep = (TBEntry_pawn *)tbe;
                for (u08 f = 0; f < 4; ++f)
                {
                    if (tbep->file[f].precomp[0] != nullptr)
                    {
                        free (tbep->file[f].precomp[0]);
                    }
                    if (tbep->file[f].precomp[1] != nullptr)
                    {
                        free (tbep->file[f].precomp[1]);
                    }
                }
            }
            else
            {
                auto tbep = (TBEntry_piece *)tbe;
                if (tbep->precomp[0] != nullptr)
                {
                    free (tbep->precomp[0]);
                }
                if (tbep->precomp[1] != nullptr)
                {
                    free (tbep->precomp[1]);
                }
            }
            free (tbe);
        }

        void free_dtz_entry (TBEntry *tbe)
        {
            unmap_file (tbe->data, tbe->mapping);
            if (tbe->has_pawns)
            {
                auto tbep = (DTZEntry_pawn *)tbe;
                for (u08 f = 0; f < 4; ++f)
                {
                    if (tbep->file[f].precomp != nullptr)
                    {
                        free (tbep->file[f].precomp);
                    }
                }
            }
            else
            {
                auto tbep = (DTZEntry_piece *)tbe;
                if (tbep->precomp != nullptr)
                {
                    free (tbep->precomp);
                }
            }
            free (tbe);
        }

        i32 wdl_to_map[5] ={ 1, 3, 0, 2, 0 };
        u08 pa_flags[5] ={ 8, 0, 0, 0, 4 };
    }

    namespace {

        bool Initialized = false;
        char *PathString = nullptr;

        // Given a position with 6 or fewer pieces, produce a text string
        // of the form KQPvKRP, where "KQP" represents the white pieces if
        // mirror == 0 and the black pieces if mirror == 1.
        void prt_str (Position &pos, char *str, i32 mirror)
        {
            Color color;
            PieceT pt;
            i32 i;

            color = !mirror ? WHITE : BLACK;
            for (pt = KING; pt >= PAWN; --pt)
            {
                for (i = pop_count<MAX15> (pos.pieces (color, pt)); i > 0; --i)
                {
                    *str++ = PieceChar[NONE - (pt + 1)];
                }
            }
            *str++ = 'v';
            color = ~color;
            for (pt = KING; pt >= PAWN; --pt)
            {
                for (i = pop_count<MAX15> (pos.pieces (color, pt)); i > 0; --i)
                {
                    *str++ = PieceChar[NONE - (pt + 1)];
                }
            }
            *str++ = 0;
        }

        // Given a position, produce a 64-bit material signature key.
        // If the engine supports such a key, it should equal the engine's key.
        u64 calc_key (Position &pos, i32 mirror)
        {
            Color color;
            PieceT pt;
            i32 i;
            u64 key = 0;

            color = !mirror ? WHITE : BLACK;
            for (pt = PAWN; pt <= KING; ++pt)
            {
                for (i = pop_count<MAX15> (pos.pieces (color, pt)); i > 0; --i)
                {
                    key ^= Zob._.piece_square[WHITE][pt][i - 1];
                }
            }
            color = ~color;
            for (pt = PAWN; pt <= KING; ++pt)
            {
                for (i = pop_count<MAX15> (pos.pieces (color, pt)); i > 0; --i)
                {
                    key ^= Zob._.piece_square[BLACK][pt][i - 1];
                }
            }
            return key;
        }

        // Produce a 64-bit material key corresponding to the material combination
        // defined by pcs[16], where pcs[1], ..., pcs[6] is the number of white
        // pawns, ..., kings and pcs[9], ..., pcs[14] is the number of black
        // pawns, ..., kings.
        u64 calc_key_from_pcs (u08 *pcs, bool mirror)
        {
            i32 color;
            PieceT pt;
            i32 i;
            u64 key = 0;

            color = mirror ? 8 : 0;
            for (pt = PAWN; pt <= KING; ++pt)
            {
                for (i = 0; i < pcs[color + (pt + 1)]; ++i)
                {
                    key ^= Zob._.piece_square[WHITE][pt][i];
                }
            }
            color ^= 8;
            for (pt = PAWN; pt <= KING; ++pt)
            {
                for (i = 0; i < pcs[color + (pt + 1)]; ++i)
                {
                    key ^= Zob._.piece_square[BLACK][pt][i];
                }
            }
            return key;
        }

        bool is_little_endian ()
        {
            union {
                i32 i;
                char c[sizeof (i32)];
            } x;
            x.i = 1;
            return x.c[0] == 1;
        }

        u08 decompress_pairs (PairsData *d, u64 idx)
        {
            static const bool isLittleEndian = is_little_endian ();
            return isLittleEndian ? decompress_pairs<true > (d, idx)
                : decompress_pairs<false> (d, idx);
        }

        // probe_wdl_table and probe_dtz_table require similar adaptations.
        i32 probe_wdl_table (Position &pos, i32 *success)
        {
            TBEntry *ptr;
            TBHashEntry *ptr2;
            u64 idx;
            u64 key;
            i32 i;
            u08 res;
            i32 p[NONE];

            // Obtain the position's material signature key.
            key = pos.matl_key ();

            // Test for KvK.
            if (key == (Zob._.piece_square[WHITE][KING][0] ^ Zob._.piece_square[BLACK][KING][0]))
                return 0;

            ptr2 = TB_hash[key >> (64 - TBHASHBITS)];
            for (i = 0; i < HSHMAX; ++i)
                if (ptr2[i].key == key) break;
            if (i == HSHMAX)
            {
                *success = 0;
                return 0;
            }

            ptr = ptr2[i].tbe;
            if (!ptr->ready)
            {
                LOCK (TB_mutex);
                if (!ptr->ready)
                {
                    char str[16];
                    prt_str (pos, str, ptr->key != key);
                    if (!init_table_wdl (ptr, str))
                    {
                        ptr2[i].key = 0ULL;
                        *success = 0;
                        UNLOCK (TB_mutex);
                        return 0;
                    }
                    // Memory barrier to ensure ptr->ready = 1 is not reordered.
#ifdef _MSC_VER
                    _ReadWriteBarrier ();
#else
                    __asm__ __volatile__ ("" ::: "memory");
#endif
                    ptr->ready = 1;
                }
                UNLOCK (TB_mutex);
            }

            i32 bside, mirror, cmirror;
            if (!ptr->symmetric)
            {
                if (key != ptr->key)
                {
                    cmirror = 8;
                    mirror = 0x38;
                    bside = (pos.active () == WHITE);
                }
                else
                {
                    cmirror = mirror = 0;
                    bside = !(pos.active () == WHITE);
                }
            }
            else
            {
                cmirror = pos.active () == WHITE ? 0 : 8;
                mirror = pos.active () == WHITE ? 0 : 0x38;
                bside = 0;
            }

            // p[i] is to contain the square 0-63 (A1-H8) for a piece of type
            // pc[i] ^ cmirror, where 1 = white pawn, ..., 14 = black king.
            // Pieces of the same type are guaranteed to be consecutive.
            if (ptr->has_pawns)
            {
                TBEntry_pawn *entry = (TBEntry_pawn *)ptr;
                i32 k = entry->file[0].pieces[0][0] ^ cmirror;
                Bitboard bb = pos.pieces ((Color)(k >> 3), (PieceT)(k & 0x07));
                i = 0;
                do {
                    p[i++] = pop_lsq (bb) ^ mirror;
                } while (bb);
                i32 f = pawn_file (entry, p);
                u08 *pc = entry->file[f].pieces[bside];
                for (; i < entry->num;)
                {
                    bb = pos.pieces ((Color)((pc[i] ^ cmirror) >> 3),
                        (PieceT)(pc[i] & 0x07));
                    do
                    {
                        if (i < NONE) p[i++] = pop_lsq (bb) ^ mirror;
                    } while (bb);
                }
                idx = encode_pawn (entry, entry->file[f].norm[bside], p, entry->file[f].factor[bside]);
                res = decompress_pairs (entry->file[f].precomp[bside], idx);
            }
            else
            {
                TBEntry_piece *entry = (TBEntry_piece *)ptr;
                u08 *pc = entry->pieces[bside];
                for (i = 0; i < entry->num;)
                {
                    Bitboard bb = pos.pieces ((Color)((pc[i] ^ cmirror) >> 3), (PieceT)(pc[i] & 0x07));
                    do
                    {
                        if (i < 6) p[i++] = pop_lsq (bb);
                    } while (bb);
                }
                idx = encode_piece (entry, entry->norm[bside], p, entry->factor[bside]);
                res = decompress_pairs (entry->precomp[bside], idx);
            }
            return ((i32)res) - 2;
        }

        i32 probe_dtz_table (Position &pos, i32 wdl, i32 *success)
        {
            u64 idx;
            i32 i, res;
            i32 p[NONE];

            //for (u08 i = 0; i < NONE; ++i) p[i] = 0x00;
            memset (p, 0x00, sizeof (p));

            // Obtain the position's material signature key.
            u64 key = pos.matl_key ();

            if (DTZ_table[0].key1 != key && DTZ_table[0].key2 != key)
            {
                for (i = 1; i < DTZ_ENTRIES; ++i)
                {
                    if (DTZ_table[i].key1 == key) break;
                }
                if (i < DTZ_ENTRIES)
                {
                    DTZTableEntry table_entry = DTZ_table[i];
                    for (; i > 0; --i)
                    {
                        DTZ_table[i] = DTZ_table[i - 1];
                    }
                    DTZ_table[0] = table_entry;
                }
                else
                {
                    auto tbhe = TB_hash[key >> (64 - TBHASHBITS)];
                    for (i = 0; i < HSHMAX; ++i)
                    {
                        if (tbhe[i].key == key)
                            break;
                    }
                    if (i == HSHMAX)
                    {
                        *success = 0;
                        return 0;
                    }
                    auto tbe = tbhe[i].tbe;
                    char str[16];
                    i32 mirror = (tbe->key != key);
                    prt_str (pos, str, mirror);
                    if (DTZ_table[DTZ_ENTRIES - 1].tbe != nullptr)
                    {
                        free_dtz_entry (DTZ_table[DTZ_ENTRIES-1].tbe);
                    }
                    for (i = DTZ_ENTRIES - 1; i > 0; --i)
                    {
                        DTZ_table[i] = DTZ_table[i - 1];
                    }
                    load_dtz_table (str, calc_key (pos, mirror), calc_key (pos, !mirror));
                }
            }

            auto tbe = DTZ_table[0].tbe;
            if (tbe == nullptr)
            {
                *success = 0;
                return 0;
            }

            i32 bside, mirror, cmirror;
            if (!tbe->symmetric)
            {
                if (key != tbe->key)
                {
                    cmirror = 8;
                    mirror = 0x38;
                    bside = (pos.active () == WHITE);
                }
                else
                {
                    cmirror = mirror = 0;
                    bside = !(pos.active () == WHITE);
                }
            }
            else
            {
                cmirror = pos.active () == WHITE ? 0 : 8;
                mirror = pos.active () == WHITE ? 0 : 0x38;
                bside = 0;
            }

            if (tbe->has_pawns)
            {
                auto dtzep = (DTZEntry_pawn *)tbe;
                i32 k = dtzep->file[0].pieces[0] ^ cmirror;
                Bitboard bb = pos.pieces ((Color)(k >> 3), (PieceT)(k & 0x07));
                i = 0;
                do {
                    p[i++] = pop_lsq (bb) ^ mirror;
                } while (bb);
                i32 f = pawn_file ((TBEntry_pawn *)dtzep, p);
                if ((dtzep->flags[f] & 1) != bside)
                {
                    *success = -1;
                    return 0;
                }
                u08 *pc = dtzep->file[f].pieces;
                for (; i < dtzep->num;)
                {
                    bb = pos.pieces ((Color)((pc[i] ^ cmirror) >> 3), (PieceT)(pc[i] & 0x07));
                    do
                    {
                        if (i < NONE) p[i++] = pop_lsq (bb) ^ mirror;
                    } while (bb);
                }
                idx = encode_pawn ((TBEntry_pawn *)dtzep, dtzep->file[f].norm, p, dtzep->file[f].factor);
                res = decompress_pairs (dtzep->file[f].precomp, idx);

                if (dtzep->flags[f] & 2)
                    res = dtzep->map[dtzep->map_idx[f][wdl_to_map[wdl + 2]] + res];

                if (!(dtzep->flags[f] & pa_flags[wdl + 2]) || (wdl & 1))
                    res *= 2;
            }
            else
            {
                DTZEntry_piece *entry = (DTZEntry_piece *)tbe;
                if ((entry->flags & 1) != bside && !entry->symmetric)
                {
                    *success = -1;
                    return 0;
                }
                u08 *pc = entry->pieces;
                for (i = 0; i < entry->num;)
                {
                    Bitboard bb = pos.pieces ((Color)((pc[i] ^ cmirror) >> 3), (PieceT)(pc[i] & 0x07));
                    do
                    {
                        if (i < NONE) p[i++] = pop_lsq (bb);
                    } while (bb);
                }
                idx = encode_piece ((TBEntry_piece *)entry, entry->norm, p, entry->factor);
                res = decompress_pairs (entry->precomp, idx);

                if (entry->flags & 2)
                    res = entry->map[entry->map_idx[wdl_to_map[wdl + 2]] + res];

                if (!(entry->flags & pa_flags[wdl + 2]) || (wdl & 1))
                    res *= 2;
            }
            return res;
        }

        // Add underpromotion captures to list of captures.
        ValMove *generate_underprom_cap (Position &pos, ValMove *moves, ValMove *end)
        {
            ValMove *cur, *extra = end;
            for (cur = moves; cur < end; ++cur)
            {
                auto move = cur->move;
                if (   mtype (move) == PROMOTE
                    && !pos.empty (dst_sq (move))
                   )
                {
                    (*extra++).move = Move(move - (NIHT << 12));
                    (*extra++).move = Move(move - (BSHP << 12));
                    (*extra++).move = Move(move - (ROOK << 12));
                }
            }

            return extra;
        }

        i32 probe_ab (Position &pos, i32 alpha, i32 beta, i32 *success)
        {
            ValMove moves[64];
            ValMove *end;
            
            // Generate (at least) all legal non-ep captures including (under)promotions.
            // It is OK to generate more, as long as they are filtered out below.
            if (pos.checkers () == U64 (0))
            {
                end = generate<CAPTURE> (moves, pos);
                // Since underpromotion captures are not included, we need to add them.
                end = generate_underprom_cap (pos, moves, end);
            }
            else
            {
                end = generate<EVASION> (moves, pos);
            }

            i32 v;
            CheckInfo ci (pos);
            ValMove *cur;
            for (cur = moves; cur < end; ++cur)
            {
                Move capture = cur->move;
                if (   !pos.capture (capture)
                    || mtype (capture) == ENPASSANT
                    || !pos.legal (capture, ci.pinneds)
                   )
                {
                    continue;
                }

                StateInfo si;
                pos.do_move (capture, si, pos.gives_check (capture, ci));
                v = -probe_ab (pos, -beta, -alpha, success);
                pos.undo_move ();
                if (*success == 0) return 0;
                if (v > alpha)
                {
                    if (v >= beta)
                    {
                        *success = 2;
                        return v;
                    }
                    alpha = v;
                }
            }

            v = probe_wdl_table (pos, success);
            if (*success == 0) return 0;
            if (alpha >= v)
            {
                *success = 1 + (alpha > 0);
                return alpha;
            }
            else
            {
                *success = 1;
                return v;
            }
        }

        // This routine treats a position with en passant captures as one without.
        i32 probe_dtz_no_ep (Position &pos, i32 *success)
        {
            i32 wdl, dtz;

            wdl = probe_ab (pos, -2, 2, success);
            if (*success == 0) return 0;

            if (wdl == 0) return 0;

            if (*success == 2)
            {
                return wdl == 2 ? 1 : 101;
            }
            ValMove stack[MAX_MOVES];
            ValMove *moves, *end = nullptr;
            StateInfo st;
            CheckInfo ci (pos);

            if (wdl > 0)
            {
                // Generate at least all legal non-capturing pawn moves
                // including non-capturing promotions.
                if (!pos.checkers ())
                {
                    end = generate<RELAX> (stack, pos);
                }
                else
                {
                    end = generate<EVASION> (stack, pos);
                }
                for (moves = stack; moves < end; ++moves)
                {
                    Move move = moves->move;
                    if (ptype (pos[org_sq (move)]) != PAWN
                        || pos.capture (move)
                        || !pos.legal (move, ci.pinneds)
                        )
                    {
                        continue;
                    }
                    pos.do_move (move, st, pos.gives_check (move, ci));
                    i32 v = -probe_ab (pos, -2, -wdl + 1, success);
                    pos.undo_move ();
                    if (*success == 0) return 0;
                    if (v == wdl)
                    {
                        return v == 2 ? 1 : 101;
                    }
                }
            }

            dtz = 1 + probe_dtz_table (pos, wdl, success);
            if (*success >= 0)
            {
                if (wdl & 1) dtz += 100;
                return wdl >= 0 ? dtz : -dtz;
            }

            if (wdl > 0)
            {
                i32 best = 0xFFFF;
                for (moves = stack; moves < end; ++moves)
                {
                    Move move = moves->move;
                    if (   pos.capture (move)
                        || ptype (pos[org_sq (move)]) == PAWN
                        || !pos.legal (move, ci.pinneds)
                       )
                    {
                        continue;
                    }
                    pos.do_move (move, st, pos.gives_check (move, ci));
                    i32 v = -probe_dtz (pos, success);
                    pos.undo_move ();
                    if (*success == 0) return 0;
                    if (v > 0 && best > v + 1)
                    {
                        best = v + 1;
                    }
                }
                return best;
            }
            else
            {
                i32 best = -1;
                if (!pos.checkers ())
                {
                    end = generate<RELAX> (stack, pos);
                }
                else
                {
                    end = generate<EVASION> (stack, pos);
                }
                for (moves = stack; moves < end; ++moves)
                {
                    i32 v;
                    Move move = moves->move;
                    if (!pos.legal (move, ci.pinneds)) continue;
                    pos.do_move (move, st, pos.gives_check (move, ci));
                    if (st.clock_ply == 0)
                    {
                        if (wdl == -2) v = -1;
                        else
                        {
                            v = probe_ab (pos, 1, 2, success);
                            v = (v == 2) ? 0 : -101;
                        }
                    }
                    else
                    {
                        v = -probe_dtz (pos, success) - 1;
                    }
                    pos.undo_move ();
                    if (*success == 0) return 0;
                    if (best > v)
                    {
                        best = v;
                    }
                }
                return best;
            }
        }

        // Check whether there has been at least one repetition of positions
        // since the last capture or pawn move.
        i32 has_repeated (StateInfo *st)
        {
            while (true)
            {
                i32 i = 4, e = std::min (st->clock_ply, st->null_ply);
                if (e < i)
                {
                    return 0;
                }
                StateInfo *stp = st->ptr->ptr;
                do
                {
                    stp = stp->ptr->ptr;
                    if (stp->posi_key == st->posi_key)
                    {
                        return 1;
                    }
                    i += 2;
                } while (i <= e);
                st = st->ptr;
            }
        }

        i32 Wdl_to_Dtz[] =
        {
            -1, -101, 0, 101, 1
        };


        Value Wdl_to_Value[5] =
        {
            -VALUE_MATE + i32(MAX_DEPTH) + 1,
            VALUE_DRAW - 2,
            VALUE_DRAW,
            VALUE_DRAW + 2,
            VALUE_MATE - i32(MAX_DEPTH) - 1
        };
    }

    // Probe the DTZ table for a particular position.
    // If *success != 0, the probe was successful.
    // The return value is from the point of view of the side to move:
    //         n < -100 : loss, but draw under 50-move rule
    // -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
    //         0        : draw
    //     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
    //   100 < n        : win, but draw under 50-move rule
    //
    // The return value n can be off by 1: a return value -n can mean a loss
    // in n+1 ply and a return value +n can mean a win in n+1 ply. This
    // cannot happen for tables with positions exactly on the "edge" of
    // the 50-move rule.
    //
    // This implies that if dtz > 0 is returned, the position is certainly
    // a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
    // picks moves that preserve dtz + 50-move-counter <= 99.
    //
    // If n = 100 immediately after a capture or pawn move, then the position
    // is also certainly a win, and during the whole phase until the next
    // capture or pawn move, the inequality to be preserved is
    // dtz + 50-movecounter <= 100.
    //
    // In short, if a move is available resulting in dtz + 50-move-counter <= 99,
    // then do not accept moves leading to dtz + 50-move-counter == 100.
    //
    i32 probe_dtz (Position &pos, i32 *success)
    {
        *success = 1;
        i32 v = probe_dtz_no_ep (pos, success);

        if (pos.en_passant_sq () == SQ_NO) return v;
        if (*success == 0) return 0;

        // Now handle en passant.
        i32 v1 = -3;

        ValMove stack[MAX_MOVES];
        ValMove *moves, *end;
        StateInfo st;

        if (!pos.checkers ())
        {
            end = generate<CAPTURE> (stack, pos);
        }
        else
        {
            end = generate<EVASION> (stack, pos);
        }
        CheckInfo ci (pos);

        for (moves = stack; moves < end; ++moves)
        {
            Move capture = moves->move;
            if (mtype (capture) != ENPASSANT
                || !pos.legal (capture, ci.pinneds)
                )
            {
                continue;
            }
            pos.do_move (capture, st, pos.gives_check (capture, ci));
            i32 v0 = -probe_ab (pos, -2, 2, success);
            pos.undo_move ();
            if (*success == 0) return 0;
            if (v1 < v0) v1 = v0;
        }
        if (v1 > -3)
        {
            v1 = Wdl_to_Dtz[v1 + 2];
            if (v < -100)
            {
                if (v1 >= 0)
                    v = v1;
            }
            else if (v < 0)
            {
                if (v1 >= 0 || v1 < -100)
                    v = v1;
            }
            else if (v > 100)
            {
                if (v1 > 0)
                    v = v1;
            }
            else if (v > 0)
            {
                if (v1 == 1)
                    v = v1;
            }
            else if (v1 >= 0)
            {
                v = v1;
            }
            else
            {
                for (moves = stack; moves < end; ++moves)
                {
                    Move move = moves->move;
                    if (mtype (move) == ENPASSANT) continue;
                    if (pos.legal (move, ci.pinneds)) break;
                }
                if (moves == end && !pos.checkers ())
                {
                    end = generate<QUIET> (end, pos);
                    for (; moves < end; ++moves)
                    {
                        Move move = moves->move;
                        if (pos.legal (move, ci.pinneds)) break;
                    }
                }
                if (moves == end)
                    v = v1;
            }
        }

        return v;
    }

    // Probe the WDL table for a particular position.
    // If *success != 0, the probe was successful.
    // The return value is from the point of view of the side to move:
    // -2 : loss
    // -1 : loss, but draw under 50-move rule
    //  0 : draw
    //  1 : win, but draw under 50-move rule
    //  2 : win
    i32 probe_wdl (Position &pos, i32 *success)
    {
        *success = 1;
        i32 v = probe_ab (pos, -2, 2, success);

        // If en passant is not possible, we are done.
        if (pos.en_passant_sq () == SQ_NO)
        {
            return v;
        }
        if (!(*success)) return 0;

        // Now handle en passant.
        i32 v1 = -3;
        // Generate (at least) all legal en passant captures.
        ValMove moves[MAX_MOVES];
        ValMove *end = pos.checkers () != U64 (0) ?
            generate<EVASION> (moves, pos) :
            generate<CAPTURE> (moves, pos);

        CheckInfo ci (pos);
        
        ValMove *cur;
        for (cur = moves; cur < end; ++cur)
        {
            Move capture = cur->move;
            if (   mtype (capture) != ENPASSANT
                || !pos.legal (capture, ci.pinneds)
               )
            {
                continue;
            }
            StateInfo si;
            pos.do_move (capture, si, pos.gives_check (capture, ci));
            i32 v0 = -probe_ab (pos, -2, 2, success);
            pos.undo_move ();
            if (*success == 0) return 0;
            if (v0 > v1) v1 = v0;
        }
        if (v1 > -3)
        {
            if (v <= v1)
            {
                v = v1;
            }
            else if (v == 0)
            {
                // Check whether there is at least one legal non-ep move.
                for (cur = moves; cur < end; ++cur)
                {
                    Move capture = cur->move;
                    if (mtype (capture) == ENPASSANT) continue;
                    if (pos.legal (capture, ci.pinneds)) break;
                }
                if (cur == end && pos.checkers () == U64(0))
                {
                    end = generate<QUIET> (end, pos);
                    for (; cur < end; ++cur)
                    {
                        Move move = cur->move;
                        if (pos.legal (move, ci.pinneds))
                            break;
                    }
                }
                // If not, then we are forced to play the losing ep capture.
                if (cur == end)
                {
                    v = v1;
                }
            }
        }

        return v;
    }

    // Use the DTZ tables to filter out moves that don't preserve the win or draw.
    // If the position is lost, but DTZ is fairly high, only keep moves that
    // maximise DTZ.
    //
    // A return value false indicates that not all probes were successful and that
    // no moves were filtered out.
    bool root_probe_dtz (Position &pos, RootMoveVector &root_moves)
    {
        i32 success;

        i32 dtz = probe_dtz (pos, &success);
        if (!success) return false;

        StateInfo st;
        CheckInfo ci (pos);

        // Probe each move.
        for (size_t i = 0; i < root_moves.size (); ++i)
        {
            Move move = root_moves[i].pv[0];
            pos.do_move (root_moves[i].pv[0], st, pos.gives_check (move, ci));
            i32 v = 0;
            if (pos.checkers () && dtz > 0)
            {
                ValMove s[192];
                if (generate<LEGAL> (s, pos) == s)
                    v = 1;
            }
            if (!v)
            {
                if (st.clock_ply != 0)
                {
                    v = -probe_dtz (pos, &success);
                    if (v > 0) v++;
                    else if (v < 0) v--;
                }
                else
                {
                    v = -probe_wdl (pos, &success);
                    v = Wdl_to_Dtz[v + 2];
                }
            }
            pos.undo_move ();
            if (!success) return false;
            root_moves[i].new_value = Value (v);
        }

        // Obtain 50-move counter for the root position.
        // In Stockfish there seems to be no clean way, so we do it like this:
        i32 cnt50 = st.ptr->clock_ply;

        // Use 50-move counter to determine whether the root position is
        // won, lost or drawn.
        i32 wdl = 0;
        if (dtz > 0)
            wdl = (dtz + cnt50 <= 100) ? 2 : 1;
        else if (dtz < 0)
            wdl = (-dtz + cnt50 <= 100) ? -2 : -1;

        // Determine the score to report to the user.
        ProbeValue = Wdl_to_Value[wdl + 2];
        // If the position is winning or losing, but too few moves left, adjust the
        // score to show how close it is to winning or losing.
        // NOTE: i32(PawnValueEg) is used as scaling factor in score_to_uci().
        if (wdl == 1 && dtz <= 100)
        {
            ProbeValue = Value (((200 - dtz - cnt50) * i32(VALUE_EG_PAWN)) / 200);
        }
        else if (wdl == -1 && dtz >= -100)
        {
            ProbeValue = -Value (((200 + dtz - cnt50) * i32(VALUE_EG_PAWN)) / 200);
        }
        // Now be a bit smart about filtering out moves.
        size_t j = 0;
        if (dtz > 0)
        { // winning (or 50-move rule draw)
            i32 best = 0xFFFF;
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                i32 v = root_moves[i].new_value;
                if (v > 0 && best > v)
                {
                    best = v;
                }
            }
            i32 max = best;
            // If the current phase has not seen repetitions, then try all moves
            // that stay safely within the 50-move budget, if there are any.
            if (!has_repeated (st.ptr) && best + cnt50 <= 99)
                max = 99 - cnt50;
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                i32 v = root_moves[i].new_value;
                if (v > 0 && v <= max)
                    root_moves[j++] = root_moves[i];
            }
        }
        else if (dtz < 0)
        { // losing (or 50-move rule draw)
            i32 best = 0;
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                i32 v = root_moves[i].new_value;
                if (v < best)
                    best = v;
            }
            // Try all moves, unless we approach or have a 50-move rule draw.
            if (-best * 2 + cnt50 < 100)
                return true;
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                if (root_moves[i].new_value == best)
                    root_moves[j++] = root_moves[i];
            }
        }
        else
        { // drawing
       // Try all moves that preserve the draw.
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                if (root_moves[i].new_value == 0)
                    root_moves[j++] = root_moves[i];
            }
        }
        root_moves.resize (j, RootMove (MOVE_NONE));

        return true;
    }

    // Use the WDL tables to filter out moves that don't preserve the win or draw.
    // This is a fallback for the case that some or all DTZ tables are missing.
    //
    // A return value false indicates that not all probes were successful and that
    // no moves were filtered out.
    bool root_probe_wdl (Position &pos, RootMoveVector &root_moves)
    {
        i32 success;

        i32 wdl = probe_wdl (pos, &success);
        if (!success) return false;
        ProbeValue = Wdl_to_Value[wdl + 2];

        StateInfo st;
        CheckInfo ci (pos);

        i32 best = -2;

        // Probe each move.
        for (size_t i = 0; i < root_moves.size (); ++i)
        {
            Move move = root_moves[i][0];
            pos.do_move (move, st, pos.gives_check (move, ci));
            i32 v = -probe_wdl (pos, &success);
            pos.undo_move ();
            if (!success) return false;
            root_moves[i].new_value = Value (v);
            if (best < v)
            {
                best = v;
            }
        }

        size_t j = 0;
        for (size_t i = 0; i < root_moves.size (); ++i)
        {
            if (root_moves[i].new_value == best)
            {
                root_moves[j++] = root_moves[i];
            }
        }
        root_moves.resize (j, RootMove ());

        return true;
    }

    void initialize (const string syzygy_path)
    {
        if (white_spaces (syzygy_path) || syzygy_path == "<empty>") return;

        if (Initialized)
        {
            free (PathString);
            free (Paths);
            
            for (u32 i = 0; i < TB_piece_count; ++i)
            {
                auto tbe = (TBEntry *)&TB_piece[i];
                free_wdl_entry (tbe);
            }
            for (u32 i = 0; i < TB_pawn_count; ++i)
            {
                auto tbe = (TBEntry *)&TB_pawn[i];
                free_wdl_entry (tbe);
            }
            for (u32 i = 0; i < DTZ_ENTRIES; ++i)
            {
                if (DTZ_table[i].tbe != nullptr)
                {
                    free_dtz_entry (DTZ_table[i].tbe);
                }
            }
        }
        else
        {
            init_indices ();
            Initialized = true;
        }

        PathString = strdup (syzygy_path.c_str ());
        PathCount = 0;
        
        i32 i = 0;
        while (i < i32(syzygy_path.length ()))
        {
            while (PathString[i] != '\0' && isspace (PathString[i])) PathString[i++] = '\0';
            if (PathString[i] == '\0') break;
            if (PathString[i] != SEP_CHAR) ++PathCount;
            while (PathString[i] != '\0' && PathString[i] != SEP_CHAR) ++i;
            if (PathString[i] == '\0') break;
            PathString[i++] = '\0';
        }

        Paths = (char **)malloc (PathCount * sizeof (char *));
        for (i32 n = i = 0; n < PathCount; ++n)
        {
            while (PathString[i] == '\0') ++i;
            Paths[n] = &PathString[i];
            while (PathString[i] != '\0') ++i;
        }

        LOCK_INIT (TB_mutex);

        TB_piece_count  = 0;
        TB_pawn_count   = 0;
        MaxPieceLimit   = 0;

        i32 j, k, l;

        for (i = 0; i < (1 << TBHASHBITS); ++i)
        {
            for (j = 0; j < HSHMAX; ++j)
            {
                TB_hash[i][j].key = 0ULL;
                TB_hash[i][j].tbe = nullptr;
            }
        }
        for (i = 0; i < DTZ_ENTRIES; ++i)
        {
            DTZ_table[i].tbe = nullptr;
        }

        char filename[16];
        for (i = 1; i < NONE; ++i)
        {
            sprintf (filename, "K%cvK", PieceChar[i]);
            init_tb (filename);
        }

        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                sprintf (filename, "K%cvK%c", PieceChar[i], PieceChar[j]);
                init_tb (filename);
            }
        }
        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                sprintf (filename, "K%c%cvK", PieceChar[i], PieceChar[j]);
                init_tb (filename);
            }
        }
        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                for (k = 1; k < NONE; ++k)
                {
                    sprintf (filename, "K%c%cvK%c", PieceChar[i], PieceChar[j], PieceChar[k]);
                    init_tb (filename);
                }
            }
        }
        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                for (k = j; k < NONE; ++k)
                {
                    sprintf (filename, "K%c%c%cvK", PieceChar[i], PieceChar[j], PieceChar[k]);
                    init_tb (filename);
                }
            }
        }
        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                for (k = i; k < NONE; ++k)
                {
                    for (l = (i == k) ? j : k; l < NONE; ++l)
                    {
                        sprintf (filename, "K%c%cvK%c%c", PieceChar[i], PieceChar[j], PieceChar[k], PieceChar[l]);
                        init_tb (filename);
                    }
                }
            }
        }
        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                for (k = j; k < NONE; ++k)
                {
                    for (l = 1; l < NONE; ++l)
                    {
                        sprintf (filename, "K%c%c%cvK%c", PieceChar[i], PieceChar[j], PieceChar[k], PieceChar[l]);
                        init_tb (filename);
                    }
                }
            }
        }
        for (i = 1; i < NONE; ++i)
        {
            for (j = i; j < NONE; ++j)
            {
                for (k = j; k < NONE; ++k)
                {
                    for (l = k; l < NONE; ++l)
                    {
                        sprintf (filename, "K%c%c%c%cvK", PieceChar[i], PieceChar[j], PieceChar[k], PieceChar[l]);
                        init_tb (filename);
                    }
                }
            }
        }

        std::cout << "info string " << (TB_piece_count + TB_pawn_count) << " Syzygy Tablebases found." << std::endl;
    }
}