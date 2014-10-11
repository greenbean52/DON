#include "Pawns.h"

namespace Pawns {

    using namespace std;
    using namespace BitBoard;

    namespace {

    #define V Value

        // Weakness of our pawn shelter in front of the king indexed by [rank]
        const Value SHELTER_WEAKNESS_VALUES[R_NO] =
        {
            V(+100), V(+  0), V(+ 27), V(+ 73), V(+ 92), V(+101), V(+101), V(+  0)
        };

        // Danger of enemy pawns moving toward our king indexed by
        // [no friendly pawn | pawn unblocked | pawn blocked][rank of enemy pawn]
        const Value STORM_DANGER_VALUES[3][R_NO] =
        {
            { V(+ 0),  V(+64), V(+128), V(+51), V(+26),  V(+ 0),  V(+ 0),  V(+ 0) },
            { V(+26),  V(+32), V(+ 96), V(+38), V(+20),  V(+ 0),  V(+ 0),  V(+ 0) },
            { V(+ 0),  V(+ 0), V(+160), V(+25), V(+13),  V(+ 0),  V(+ 0),  V(+ 0) }
        };

        // Max bonus for king safety by pawns.
        // Corresponds to start position with all the pawns
        // in front of the king and no enemy pawn on the horizon.
        const Value PAWN_KING_SAFETY_VALUE = V(+263);

    #undef V

    #define S(mg, eg) mk_score(mg, eg)

        // Connected pawn bonus by [opposed][phalanx][rank]
        const Score PAWN_CONNECTED_SCORES[2][2][R_NO] =
        {
            {
                { S(0,0), S(3, 6), S(7,15), S( 5,10), S(28,57), S(37, 75), S(67,135), S(129,258) },
                { S(1,3), S(5,10), S(6,13), S(16,33), S(33,66), S(52,105), S(98,196), S(129,258) }
            },
            {
                { S(0,0), S(3, 3), S(7, 7), S( 5, 5), S(28,28), S(37, 37), S(67, 67), S(129,129) },
                { S(1,1), S(5, 5), S(6, 6), S(16,16), S(33,33), S(52, 52), S(98, 98), S(129,129) }
            }
        };

        // Doubled pawn penalty by file
        const Score PAWN_DOUBLED_SCORES[F_NO] =
        {
            S(+13,+43), S(+20,+48), S(+23,+48), S(+23,+48),
            S(+23,+48), S(+23,+48), S(+20,+48), S(+13,+43)
        };

        // Isolated pawn penalty by [opposed][file]
        const Score PAWN_ISOLATED_SCORES[2][F_NO] =
        {
            {S(+37,+45), S(+54,+52), S(+60,+52), S(+60,+52),
            S(+60,+52), S(+60,+52), S(+54,+52), S(+37,+45)},
            {S(+25,+30), S(+36,+35), S(+40,+35), S(+40,+35),
            S(+40,+35), S(+40,+35), S(+36,+35), S(+25,+30)}
        };

        // Backward pawn penalty by [opposed][file]
        const Score PAWN_BACKWARD_SCORES[2][F_NO] =
        {
            {S(+30,+42), S(+43,+46), S(+49,+46), S(+49,+46),
            S(+49,+46), S(+49,+46), S(+43,+46), S(+30,+42)},
            {S(+20,+28), S(+29,+31), S(+33,+31), S(+33,+31),
            S(+33,+31), S(+33,+31), S(+29,+31), S(+20,+28)}
        };

        // Levers bonus by [rank]
        const Score PAWN_LEVER_SCORE[R_NO] = 
        {
            S(+ 0,+ 0), S(+ 0,+ 0), S(+ 6,+ 6), S(+12,+12),
            S(+20,+20), S(+40,+40), S(+ 0,+ 0), S(+ 0,+ 0)
        };

        const Score PAWN_SPAN_SCORE        = S(+ 0,+15); // Bonus for file distance of the two outermost pawns
        const Score PAWN_UNSTOPPABLE_SCORE = S(+ 0,+20); // Bonus for pawn going to promote
        const Score PAWN_UNSUPPORTED_SCORE = S(+20,+10); // Penalty for Unsupported pawn

        template<Color C>
        inline Score evaluate (const Position &pos, Entry *e)
        {
            const Color  C_  = WHITE == C ? BLACK  : WHITE;
            const Delta PUSH = WHITE == C ? DEL_N  : DEL_S;
            //const Delta PULL = WHITE == C ? DEL_S  : DEL_N;
            const Delta RCAP = WHITE == C ? DEL_NE : DEL_SW;
            const Delta LCAP = WHITE == C ? DEL_NW : DEL_SE;

            const Bitboard our_pawns = pos.pieces<PAWN> (C );
            const Bitboard opp_pawns = pos.pieces<PAWN> (C_);

            e->pawns_attacks  [C] = shift_del<RCAP> (our_pawns) | shift_del<LCAP> (our_pawns);
            //e->blocked_pawns  [C] = our_pawns & shift_del<PULL> (opp_pawns);
            e->passed_pawns   [C] = U64(0);
            e->semiopen_files [C] = 0xFF;
            e->king_sq        [C] = SQ_NO;

            Bitboard center_pawns = our_pawns & EXT_CENTER_bb[C];
            if (center_pawns != U64(0))
            {
                Bitboard color_pawns;
                color_pawns = center_pawns & LIHT_bb;
                e->pawns_on_sqrs[C][WHITE] = (color_pawns != U64(0) ? pop_count<MAX15>(color_pawns) : 0);
                color_pawns = center_pawns & DARK_bb;
                e->pawns_on_sqrs[C][BLACK] = (color_pawns != U64(0) ? pop_count<MAX15>(color_pawns) : 0);
            }
            else
            {
                e->pawns_on_sqrs[C][WHITE] = 0;
                e->pawns_on_sqrs[C][BLACK] = 0;
            }

            Score pawn_score = SCORE_ZERO;

            const Square *pl = pos.list<PAWN> (C);
            Square s;
            // Loop through all pawns of the current color and score each pawn
            while ((s = *pl++) != SQ_NO)
            {
                ASSERT (pos[s] == (C | PAWN));

                File f = _file (s);
                Rank r = rel_rank (C, s);
                
                // This file cannot be semi-open
                e->semiopen_files[C] &= ~(1 << f);

                Bitboard prank_bb  = rank_bb (s - PUSH);

                Bitboard adjacents = our_pawns & ADJ_FILE_bb[f];
                Bitboard supported = (adjacents & prank_bb);
                Bitboard connected = (adjacents & (prank_bb | rank_bb (s)));
                Bitboard doubled   = (our_pawns & FRONT_SQRS_bb[C][s]);

                bool phalanx       = connected & rank_bb (s);
                bool levered       = (opp_pawns & PAWN_ATTACKS[C][s]);
                bool opposed       = (opp_pawns & FRONT_SQRS_bb[C][s]);
                bool isolated      = !(adjacents);
                bool passed        = !(opp_pawns & PAWN_PASS_SPAN[C][s]);

                bool backward;
                // Test for backward pawn.
                // If the pawn is passed, isolated, connected or levered (it can capture an enemy pawn).
                // If the rank is greater then Rank 6
                // If there are friendly pawns behind on adjacent files and they are able to advance and support the pawn.
                // Then it cannot be backward either.
                if (  passed || isolated || levered || connected || r >= R_6
                   // Partially checked the opp behind pawn, But need to check own behind attack span are not backward or rammed 
                   || (our_pawns & PAWN_ATTACK_SPAN[C_][s] && !(opp_pawns & (s-PUSH)))
                   )
                {
                    backward = false;
                }
                else
                {
                    Bitboard b;
                    // Now know that there are no friendly pawns beside or behind this pawn on adjacent files.
                    // Now check whether the pawn is backward by looking in the forward direction on the
                    // adjacent files, and picking the closest pawn there.
                    b = PAWN_ATTACK_SPAN[C][s] & pos.pieces<PAWN> ();
                    b = PAWN_ATTACK_SPAN[C][s] & rank_bb (scan_backmost_sq (C, b));

                    // If have an enemy pawn in the same or next rank, the pawn is
                    // backward because it cannot advance without being captured.
                    backward = opp_pawns & (b | shift_del<PUSH> (b));
                }

                ASSERT (passed ^ (opposed || (opp_pawns & PAWN_ATTACK_SPAN[C][s])));

                // Score this pawn
                Score score = SCORE_ZERO;

                if (connected != U64(0))
                {
                    score += PAWN_CONNECTED_SCORES[opposed][phalanx][r];
                }

                if (isolated)
                {
                    score -= PAWN_ISOLATED_SCORES[opposed][f];
                }
                else
                {
                    if (supported == U64(0))
                    {
                        score -= PAWN_UNSUPPORTED_SCORE;
                    }
                    if (backward)
                    {
                        score -= PAWN_BACKWARD_SCORES[opposed][f];
                    }
                }
                
                if (r > R_4 && levered)
                {
                    score += PAWN_LEVER_SCORE[r];
                }

                if (doubled != U64(0))
                {
                    score -= PAWN_DOUBLED_SCORES[f] / i32(rank_dist (s, scan_frntmost_sq (C, doubled)));
                }
                else
                {
                    // Passed pawns will be properly scored in evaluation because need
                    // full attack info to evaluate passed pawns.
                    // Only the frontmost passed pawn on each file is considered a true passed pawn.
                    if (passed)
                    {
                        e->passed_pawns[C] += s;
                    }
                }


#ifndef NDEBUG
                //cout << to_string (s) << " : " << mg_value (score) << ", " << eg_value (score) << endl;
#endif
                pawn_score += score;
            }

#ifndef NDEBUG
            //cout << pretty (e->unstopped_pawns[C]) << endl;
            //cout << "-------------" << endl;
#endif

            // In endgame it's better to have pawns on both wings.
            // So give a bonus according to file distance between left and right outermost pawns span.
            i32 span = e->semiopen_files[C] ^ 0xFF;
            e->pawn_span[C] = span != 0 ? u08(scan_msq (span)) - u08(scan_lsq (span)) : 0;
            pawn_score += PAWN_SPAN_SCORE * i32(e->pawn_span[C]);

            return pawn_score;
        }

    #undef S

    } // namespace

    template<Color C>
    // pawn_shelter_storm() calculates shelter and storm penalties
    // for the file the king is on, as well as the two adjacent files.
    Value Entry::pawn_shelter_storm (const Position &pos, Square k_sq) const
    {
        const Color C_ = WHITE == C ? BLACK : WHITE;

        Rank kr = _rank (k_sq);
        Bitboard front_pawns = pos.pieces<PAWN> () & (FRONT_RANK_bb[C][kr] | RANK_bb[kr]);
        Bitboard our_front_pawns = front_pawns & pos.pieces (C );
        Bitboard opp_front_pawns = front_pawns & pos.pieces (C_);

        Value value = PAWN_KING_SAFETY_VALUE;

        File kf = _file (k_sq);
        i08 kfl = min (max (kf, F_B), F_G);
        for (i08 f = kfl - 1; f <= kfl + 1; ++f)
        {
            ASSERT (F_A <= f && f <= F_H);

            Bitboard mid_pawns;

            mid_pawns = opp_front_pawns & FILE_bb[f];
            u08 r1 = mid_pawns != U64(0) ? rel_rank (C, scan_frntmost_sq (C_, mid_pawns)) : R_1;
            if (  kf == f
               && END_EDGE_bb & (File(f) | Rank(r1))
               && rel_rank (C, k_sq) + 1 == r1
               )
            {
                value += Value(200); // Enemy pawn in front Shelter
            }
            else
            {
                mid_pawns = our_front_pawns & FILE_bb[f];
                u08 r0 = mid_pawns != U64(0) ? rel_rank (C, scan_backmost_sq (C , mid_pawns)) : R_1;
                value -= 
                      + SHELTER_WEAKNESS_VALUES[r0]
                      + STORM_DANGER_VALUES[r0 == R_1 ? 0 : r0 + 1 != r1 ? 1 : 2][r1];
            }
        }

        return value;
    }

    template<Color C>
    // Entry::evaluate_unstoppable_pawns<>() scores the most advanced passed pawns.
    // In case opponent has no pieces but pawns, this is somewhat
    // related to the possibility pawns are unstoppable.
    Score Entry::evaluate_unstoppable_pawns () const
    {
        return passed_pawns[C] != U64(0) ?
            PAWN_UNSTOPPABLE_SCORE * i32(rel_rank (C, scan_frntmost_sq (C, passed_pawns[C]))) :
            SCORE_ZERO;
    }

    // explicit template instantiations
    // --------------------------------
    template Value Entry::pawn_shelter_storm<WHITE> (const Position &pos, Square k_sq) const;
    template Value Entry::pawn_shelter_storm<BLACK> (const Position &pos, Square k_sq) const;

    template Score Entry::evaluate_unstoppable_pawns<WHITE> () const;
    template Score Entry::evaluate_unstoppable_pawns<BLACK> () const;

    // probe() takes a position object as input, computes a Pawn::Entry object,
    // and returns a pointer to Pawn::Entry object.
    // The result is also stored in a hash table, so don't have
    // to recompute everything when the same pawn structure occurs again.
    Entry* probe (const Position &pos, Table &table)
    {
        Key pawn_key = pos.pawn_key ();
        Entry *e     = table[pawn_key];

        if (e->pawn_key != pawn_key)
        {
            e->pawn_key   = pawn_key;
            e->pawn_score =
                + evaluate<WHITE> (pos, e)
                - evaluate<BLACK> (pos, e);
        }
        return e;
    }

} // namespace Pawns
