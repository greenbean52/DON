#include "BitBoard.h"

#include "PRNG.h"
#include "Notation.h"

namespace BitBoard {

    using namespace std;

    u08      SquareDist[SQ_NO][SQ_NO];

    Bitboard FrontSqrs_bb[CLR_NO][SQ_NO];

    Bitboard Between_bb[SQ_NO][SQ_NO];
    Bitboard StrLine_bb[SQ_NO][SQ_NO];

    Bitboard DistRings_bb[SQ_NO][8];

    Bitboard PawnAttackSpan[CLR_NO][SQ_NO];
    Bitboard PawnPassSpan[CLR_NO][SQ_NO];

    Bitboard PawnAttacks[CLR_NO][SQ_NO];
    Bitboard PieceAttacks[NONE][SQ_NO];

    Bitboard *B_Attacks_bb[SQ_NO];
    Bitboard *R_Attacks_bb[SQ_NO];

    Bitboard B_Masks_bb[SQ_NO];
    Bitboard R_Masks_bb[SQ_NO];

#if !defined(BM2)
    Bitboard B_Magics_bb[SQ_NO];
    Bitboard R_Magics_bb[SQ_NO];

    u08      B_Shifts[SQ_NO];
    u08      R_Shifts[SQ_NO];
#endif

#if !defined(ABM)
    u08 PopCount16[1 << 16];
#endif

    namespace {

        const Delta PawnDeltas[CLR_NO][3] =
        {
            { DEL_NW, DEL_NE, DEL_O },
            { DEL_SE, DEL_SW, DEL_O },
        };
        const Delta PieceDeltas[NONE][9] =
        {
            { DEL_O },
            { DEL_SSW, DEL_SSE, DEL_WWS, DEL_EES, DEL_WWN, DEL_EEN, DEL_NNW, DEL_NNE, DEL_O },
            { DEL_SW, DEL_SE, DEL_NW, DEL_NE, DEL_O },
            { DEL_S, DEL_W, DEL_E, DEL_N, DEL_O },
            { DEL_SW, DEL_S, DEL_SE, DEL_W, DEL_E, DEL_NW, DEL_N, DEL_NE, DEL_O },
            { DEL_SW, DEL_S, DEL_SE, DEL_W, DEL_E, DEL_NW, DEL_N, DEL_NE, DEL_O },
        };

//        // De Bruijn sequences. See chessprogramming.wikispaces.com/BitScan
//        const u64 DeBruijn_64 = U64(0x3F79D71B4CB0A89);
//        const u32 DeBruijn_32 = U32(0x783A9B23);
//
//        i08 MSB_Table[(1 << 8)];
//        Square BSF_Table[SQ_NO];
//
//        unsigned bsf_index (Bitboard bb)
//        {
//            assert(bb != 0);
//            bb ^= (bb - 1);
//            return
//#       if defined(BIT64)
//            // Use Kim Walisch extending trick for 64-bit
//            (bb * DeBruijn_64) >> 58;
//#       else
//            // Use Matt Taylor's folding trick for 32-bit
//            (u32 ((bb >> 0) ^ (bb >> 32)) * DeBruijn_32) >> 26;
//#       endif
//        }

    #if !defined(ABM)
        // Counts the non-zero bits using SWAR-Popcount algorithm
        u08 pop_count16 (u32 u)
        {
            u -= (u >> 1) & 0x5555U;
            u = ((u >> 2) & 0x3333U) + (u & 0x3333U);
            u = ((u >> 4) + u) & 0x0F0FU;
            return u08((u * 0x0101U) >> 8);
        }
    #endif

        // Max Bishop Table Size
        // 4 * 2^9 + 4 * 2^6 + 12 * 2^7 + 44 * 2^5
        // 4 * 512 + 4 *  64 + 12 * 128 + 44 *  32
        //    2048 +     256 +     1536 +     1408
        //                                    5248 = 0x1480
        const u32 MaxBTSize = U32(0x1480);
        Bitboard B_Tables_bb[MaxBTSize];

        // Max Rook Table Size
        // 4 * 2^12 + 24 * 2^11 + 36 * 2^10
        // 4 * 4096 + 24 * 2048 + 36 * 1024
        //    16384 +     49152 +     36864
        //                           102400 = 0x19000
        const u32 MaxRTSize = U32(0x19000);
        Bitboard R_Tables_bb[MaxRTSize];

        // Initialize all bishop and rook attacks at startup.
        // Magic bitboards are used to look up attacks of sliding pieces.
        // As a reference see chessprogramming.wikispaces.com/Magic+Bitboards.
        // In particular, here we use the so called "fancy" approach.
#   if defined(BM2)
        void initialize_table (Bitboard *const tables_bb, Bitboard **const attacks_bb, Bitboard *const masks_bb, const Delta *const deltas)
#   else
        void initialize_table (Bitboard *const tables_bb, Bitboard **const attacks_bb, Bitboard *const masks_bb, const Delta *const deltas, Bitboard *const magics_bb, u08 *const shifts, u16 (*indexer)(Square s, Bitboard occ))
#   endif
        {

#       if !defined(BM2)
            const i16 MaxIndex = 0x1000;
            Bitboard occupancy[MaxIndex]
                ,    reference[MaxIndex];

            const u32 Seeds[R_NO] =
#           if defined(BIT64)
                { 0x002D8, 0x0284C, 0x0D6E5, 0x08023, 0x02FF9, 0x03AFC, 0x04105, 0x000FF };
#           else
                { 0x02311, 0x0AE10, 0x0D447, 0x09856, 0x01663, 0x173E5, 0x199D0, 0x0427C };
#           endif

#       endif

            u32 offset = 0;
            for (auto s = SQ_A1; s <= SQ_H8; ++s)
            {
                // attacks_bb[s] is a pointer to the beginning of the attacks table for square 's'
                attacks_bb[s] = &tables_bb[offset];

                // Given a square 's', the mask is the bitboard of sliding attacks from 's'
                // computed on an empty board. The index must be big enough to contain
                // all the attacks for each possible subset of the mask and so is 2 power
                // the number of 1s of the mask. Hence deduce the size of the shift to
                // apply to the 64 or 32 bits word to get the index.
                masks_bb[s] = sliding_attacks (deltas, s)
                            // Board edges are not considered in the relevant occupancies
                            & ~(((FA_bb|FH_bb) & ~file_bb (s)) | ((R1_bb|R8_bb) & ~rank_bb (s)));

#           if !defined(BM2)
                shifts[s] =
#               if defined(BIT64)
                    64
#               else
                    32
#               endif
                    - u08(pop_count (masks_bb[s]));
#           endif

                // Use Carry-Rippler trick to enumerate all subsets of masks_bb[s] and
                // store the corresponding sliding attack bitboard in reference[].
                // Have individual table_bb sizes for each square with "Fancy Magic Bitboards".
                u32 size = 0;
                Bitboard occ = 0;
                do {
#               if defined(BM2)
                    attacks_bb[s][PEXT(occ, masks_bb[s])] = sliding_attacks (deltas, s, occ);
#               else
                    occupancy[size] = occ;
                    reference[size] = sliding_attacks (deltas, s, occ);
#               endif

                    ++size;
                    occ = (occ - masks_bb[s]) & masks_bb[s];
                } while (occ != 0);

#           if !defined(BM2)
                u32 i;
                PRNG rng (Seeds[_rank (s)]);
                
                // Find a magic for square 's' picking up an (almost) random number
                // until found the one that passes the verification test.
                do {
                    do {
                        magics_bb[s] = rng.sparse_rand<Bitboard> ();
                    } while (pop_count ((masks_bb[s] * magics_bb[s]) >> 0x38) < 6);

                    // A good magic must map every possible occupancy to an index that
                    // looks up the correct sliding attack in the attacks_bb[s] database.
                    // Note that build up the database for square 's' as a side
                    // effect of verifying the magic.
                    bool used[MaxIndex] = {false};
                    for (i = 0; i < size; ++i)
                    {
                        u16 idx = indexer (s, occupancy[i]);
                        if (used[idx])
                        {
                            if (attacks_bb[s][idx] != reference[i])
                            {
                                break;
                            }
                            continue;
                        }
                        used[idx] = true;
                        attacks_bb[s][idx] = reference[i];
                    }
                } while (i < size);
#           endif
                // Set the offset of the table_bb for the next square.
                offset += size;
            }
        }

    }

    void initialize ()
    {
        //for (auto s = SQ_A1; s <= SQ_H8; ++s)
        //{
        //    BSF_Table[bsf_index (Square_bb[s] = 1ULL << s)] = s;
        //    BSF_Table[bsf_index (Square_bb[s])] = s;
        //}
        //for (u32 b = 2; b < (1 << 8); ++b)
        //{
        //    MSB_Table[b] =  MSB_Table[b - 1] + !more_than_one (b);
        //}

    #if !defined(ABM)
        for (u32 i = 0; i < (1 << 16); ++i)
        {
            PopCount16[i] = pop_count16 (i);
        }
    #endif

        for (auto s1 = SQ_A1; s1 <= SQ_H8; ++s1)
        {
            for (auto s2 = SQ_A1; s2 <= SQ_H8; ++s2)
            {
                if (s1 != s2)
                {
                    SquareDist[s1][s2] = u08(std::max (dist<File> (s1, s2), dist<Rank> (s1, s2)));
                    DistRings_bb[s1][SquareDist[s1][s2] - 1] += s2;
                }
            }
        }

        for (auto c = WHITE; c <= BLACK; ++c)
        {
            for (auto s = SQ_A1; s <= SQ_H8; ++s)
            {
                FrontSqrs_bb  [c][s] = FrontRank_bb[c][_rank (s)] &    File_bb[_file (s)];
                PawnAttackSpan[c][s] = FrontRank_bb[c][_rank (s)] & AdjFile_bb[_file (s)];
                PawnPassSpan  [c][s] = FrontSqrs_bb[c][s] | PawnAttackSpan[c][s];
            }
        }

        for (auto s = SQ_A1; s <= SQ_H8; ++s)
        {
            u08 k;
            Delta del;

            for (auto c = WHITE; c <= BLACK; ++c)
            {
                k = 0;
                while ((del = PawnDeltas[c][k++]) != DEL_O)
                {
                    auto sq = s + del;
                    if (   _ok (sq)
                        && dist (s, sq) == 1)
                    {
                        PawnAttacks[c][s] += sq;
                    }
                }
            }

            PieceType pt;

            pt = NIHT;
            k = 0;
            while ((del = PieceDeltas[pt][k++]) != DEL_O)
            {
                auto sq = s + del;
                if (   _ok (sq)
                    && dist (s, sq) == 2)
                {
                    PieceAttacks[pt][s] += sq;
                }
            }

            pt = KING;
            k = 0;
            while ((del = PieceDeltas[pt][k++]) != DEL_O)
            {
                auto sq = s + del;
                if (   _ok (sq)
                    && dist (s, sq) == 1)
                {
                    PieceAttacks[pt][s] += sq;
                }
            }

            PieceAttacks[BSHP][s] = sliding_attacks (PieceDeltas[BSHP], s);
            PieceAttacks[ROOK][s] = sliding_attacks (PieceDeltas[ROOK], s);
            PieceAttacks[QUEN][s] = PieceAttacks[BSHP][s] | PieceAttacks[ROOK][s];
        }

        // Initialize Sliding
#       if defined(BM2)
            initialize_table (B_Tables_bb, B_Attacks_bb, B_Masks_bb, PieceDeltas[BSHP]);
            initialize_table (R_Tables_bb, R_Attacks_bb, R_Masks_bb, PieceDeltas[ROOK]);
#       else
            initialize_table (B_Tables_bb, B_Attacks_bb, B_Masks_bb, PieceDeltas[BSHP], B_Magics_bb, B_Shifts, magic_index<BSHP>);
            initialize_table (R_Tables_bb, R_Attacks_bb, R_Masks_bb, PieceDeltas[ROOK], R_Magics_bb, R_Shifts, magic_index<ROOK>);
#       endif

        // NOTE:: must be after Initialize Sliding
        for (auto s1 = SQ_A1; s1 <= SQ_H8; ++s1)
        {
            for (auto s2 = SQ_A1; s2 <= SQ_H8; ++s2)
            {
                for (auto pt = BSHP; pt <= ROOK; ++pt)
                {
                    if ((PieceAttacks[pt][s1] & s2) != 0)
                    {
                        Between_bb[s1][s2] = (attacks_bb (Piece(pt), s1, Square_bb[s2]) & attacks_bb (Piece(pt), s2, Square_bb[s1]));
                        StrLine_bb[s1][s2] = (attacks_bb (Piece(pt), s1,             0) & attacks_bb (Piece(pt), s2,             0)) + s1 + s2;
                    }
                }
            }
        }

    }

#if !defined(NDEBUG)
    // Returns an ASCII representation of a bitboard to print on console output
    // Bitboard in an easily readable format. This is sometimes useful for debugging.
    string pretty (Bitboard bb)
    {
        string s;
        s = " /---------------\\\n";
        for (auto r = R_8; r >= R_1; --r)
        {
            s += Notation::to_char (r);
            s += "|";
            for (auto f = F_A; f <= F_H; ++f)
            {
                s += (bb & (f|r) ? '+' : '-');
                if (f < F_H)
                {
                    s += " ";
                }
            }
            s += "|\n";
        }
        s += " \\---------------/\n ";
        for (auto f = F_A; f <= F_H; ++f)
        {
            s += " ";
            s += Notation::to_char (f, false);
        }
        s += '\n';
        return s;
    }
#endif

}
