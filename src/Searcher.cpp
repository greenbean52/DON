﻿#include "Searcher.h"

#include <cfloat>
#include <cmath>
#include <sstream>
#include <iomanip>

#include "TimeManager.h"
#include "Transposition.h"
#include "MoveGenerator.h"
#include "MovePicker.h"
#include "Material.h"
#include "Pawns.h"
#include "PRNG.h"
#include "Evaluator.h"
#include "Thread.h"
#include "Notation.h"
#include "Debugger.h"

using namespace std;
using namespace Time;

namespace Searcher {

    using namespace BitBoard;
    using namespace MoveGen;
    using namespace MovePick;
    using namespace Transposition;
    using namespace OpeningBook;
    using namespace Evaluator;
    using namespace Notation;
    using namespace Debug;
    using namespace UCI;

    namespace {

        const Depth FutilityMarginDepth     = Depth(7);
        // Futility margin lookup table (initialized at startup)
        // [depth]
        Value FutilityMargins[FutilityMarginDepth];

        const Depth RazorDepth     = Depth(4);
        // Razoring margin lookup table (initialized at startup)
        // [depth]
        Value RazorMargins[RazorDepth];

        const Depth FutilityMoveCountDepth  = Depth(16);
        // Futility move count lookup table (initialized at startup)
        // [improving][depth]
        u08   FutilityMoveCounts[2][FutilityMoveCountDepth];

        const Depth ReductionDepth = Depth(32);
        const u08   ReductionMoveCount = 64;
        // ReductionDepths lookup table (initialized at startup)
        // [pv][improving][depth][move_num]
        Depth ReductionDepths[2][2][ReductionDepth][ReductionMoveCount];

        template<bool PVNode>
        inline Depth reduction_depths (bool imp, Depth d, u08 mc)
        {
            return ReductionDepths[PVNode][imp][min (d, ReductionDepth-1)][min<u08> (mc, ReductionMoveCount-1)];
        }

        const Depth ProbCutDepth   = Depth(4);

        const u08   MAX_QUIETS     = 64;

        const point INFO_INTERVAL  = 3000; // 3 sec

        Color   RootColor;
        i32     RootPly;

        u08     RootSize   // RootMove Count
            ,   LimitPV
            ,   IndexPV;

        Value   DrawValue[CLR_NO]
            ,   BaseContempt[CLR_NO];

        bool    MateSearch;
        bool    SearchLogWrite;

        bool    FirstAutoSave;

        TimeManager  TimeMgr;
        // Gain statistics
        GainStats    GainStatistics;
        // History statistics
        HistoryStats HistoryStatistics;
        // Move statistics
        MoveStats     CounterMoveStats   // Counter
            ,        FollowupMoveStats;  // Followup

        // update_stats() updates history, killer, counter & followup moves
        // after a fail-high of a quiet move.
        inline void update_stats (const Position &pos, Stack *ss, Move move, Depth depth, Move *quiet_moves, u08 quiets)
        {
            if (ss->killer_moves[0] != move)
            {
                ss->killer_moves[1] = ss->killer_moves[0];
                ss->killer_moves[0] = move;
            }

            // Increase history value of the cut-off move and decrease all the other played quiet moves.
            Value value = Value((depth/DEPTH_ONE)*(depth/DEPTH_ONE));
            HistoryStatistics.update (pos, move, value);
            for (u08 i = 0; i < quiets; ++i)
            {
                HistoryStatistics.update (pos, quiet_moves[i], -value);
            }
            Move opp_move = (ss-1)->current_move;
            if (_ok (opp_move))
            {
                 CounterMoveStats.update (pos, opp_move, move);
            }
            Move own_move = (ss-2)->current_move;
            if (_ok (own_move) && opp_move == (ss-1)->tt_move)
            {
                FollowupMoveStats.update (pos, own_move, move);
            }
        }
        
        // update_pv() copies child node pv[] adding current move
        inline void update_pv (Move *pv, Move move, const Move *child_pv)
        {
            *pv++ = move;
            if (child_pv != NULL)
            {
                while (*child_pv != MOVE_NONE)
                {
                    *pv++ = *child_pv++;
                }
            }
            *pv = MOVE_NONE;
        }

        // value_to_tt() adjusts a mate score from "plies to mate from the root" to
        // "plies to mate from the current position". Non-mate scores are unchanged.
        // The function is called before storing a value to the transposition table.
        inline Value value_to_tt (Value v, i32 ply)
        {
            assert (v != VALUE_NONE);
            return v >= +VALUE_MATE_IN_MAX_DEPTH ? v + ply :
                   v <= -VALUE_MATE_IN_MAX_DEPTH ? v - ply :
                   v;
        }
        // value_of_tt() is the inverse of value_to_tt ():
        // It adjusts a mate score from the transposition table
        // (where refers to the plies to mate/be mated from current position)
        // to "plies to mate/be mated from the root".
        inline Value value_of_tt (Value v, i32 ply)
        {
            return v == VALUE_NONE               ? VALUE_NONE :
                   v >= +VALUE_MATE_IN_MAX_DEPTH ? v - ply :
                   v <= -VALUE_MATE_IN_MAX_DEPTH ? v + ply :
                   v;
        }

        // info_multipv() formats PV information according to UCI protocol.
        // UCI requires to send all the PV lines also if are still to be searched
        // and so refer to the previous search score.
        inline string info_multipv (const Position &pos, Depth depth, Value alpha, Value beta, point time)
        {
            assert (time >= 0);

            stringstream ss;

            i32 sel_depth = 0;
            for (size_t idx = 0; idx < Threadpool.size (); ++idx)
            {
                if (sel_depth < Threadpool[idx]->max_ply)
                {
                    sel_depth = Threadpool[idx]->max_ply;
                }
            }

            for (u08 i = 0; i < LimitPV; ++i)
            {
                Depth d;
                Value v;

                if (i <= IndexPV) // New updated value?
                {
                    d = depth;
                    v = RootMoves[i].new_value;
                }
                else
                {
                    if (DEPTH_ONE == depth) return "";

                    d = depth - DEPTH_ONE;
                    v = RootMoves[i].old_value;
                }

                // Not at first line
                if (ss.rdbuf ()->in_avail ()) ss << "\n";

                ss  << "info"
                    << " multipv "  << i + 1
                    << " depth "    << d/DEPTH_ONE
                    << " seldepth " << sel_depth
                    << " score "    << to_string (v)
                    << (i == IndexPV ? beta <= v ? " lowerbound" : v <= alpha ? " upperbound" : "" : "")
                    << " time "     << time
                    << " nodes "    << pos.game_nodes ()
                    << " nps "      << pos.game_nodes () * MILLI_SEC / max (time, point(1));
                if (time > MILLI_SEC) ss  << " hashfull " << TT.hash_full ();
                ss  << " pv"        << RootMoves[i].info_pv ();

            }

            return ss.str ();
        }

        template<NodeT NT, bool InCheck>
        // quien_search<>() is the quiescence search function,
        // which is called by the main depth limited search function
        // when the remaining depth is ZERO or less.
        inline Value quien_search  (Position &pos, Stack *ss, Value alpha, Value beta, Depth depth)
        {
            const bool    PVNode = NT == PV;

            assert (NT == PV || NT == NonPV);
            assert (InCheck == (pos.checkers () != U64(0)));
            assert (alpha >= -VALUE_INFINITE && alpha < beta && beta <= +VALUE_INFINITE);
            assert (PVNode || alpha == beta-1);
            assert (depth <= DEPTH_ZERO);

            ss->current_move = MOVE_NONE;
            ss->ply = (ss-1)->ply + 1;

            // Check for aborted search, immediate draw or maximum ply reached
            if (Signals.force_stop || pos.draw () || ss->ply >= MAX_DEPTH)
            {
                return ss->ply >= MAX_DEPTH && !InCheck ? evaluate (pos) : DrawValue[pos.active ()];
            }

            assert (0 <= ss->ply && ss->ply < MAX_DEPTH);

            Move  pv[MAX_DEPTH+1];
            Value pv_alpha = -VALUE_INFINITE;
            
            if (PVNode)
            {
                // To flag EXACT a node with eval above alpha and no available moves
                pv_alpha    = alpha;
                
                (ss  )->pv[0] = MOVE_NONE;
                fill (pv, pv + sizeof (pv)/sizeof (*pv), MOVE_NONE);
                (ss+1)->pv    = pv;
            }

            Move  tt_move    = MOVE_NONE
                , best_move  = MOVE_NONE;
            Value tt_value   = VALUE_NONE
                , best_value = -VALUE_INFINITE;
            Depth tt_depth   = DEPTH_NONE;
            Bound tt_bound   = BOUND_NONE;
            
            // Transposition table lookup
            Key posi_key = pos.posi_key ();
            bool  tt_hit = false;
            TTEntry *tte = TT.probe (posi_key, tt_hit);
            if (tt_hit)
            {
                tt_move  = tte->move ();
                tt_value = value_of_tt (tte->value (), ss->ply);
                tt_depth = tte->depth ();
                tt_bound = tte->bound ();
            }

            Thread *thread = pos.thread ();
            // Decide whether or not to include checks, this fixes also the type of
            // TT entry depth that are going to use. Note that in quien_search use
            // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
            Depth qs_depth = InCheck || depth >= DEPTH_QS_CHECKS ?
                                DEPTH_QS_CHECKS : DEPTH_QS_NO_CHECKS;

            CheckInfo cc, *ci = NULL;

            if (  !PVNode 
               && tt_hit
               && tt_depth >= qs_depth
               && tt_value != VALUE_NONE // Only in case of TT access race
               && (tt_value >= beta ? (tt_bound & BOUND_LOWER) : (tt_bound & BOUND_UPPER))
               )
            {
                ss->current_move = tt_move; // Can be MOVE_NONE
                return tt_value;
            }

            Value futility_base = -VALUE_INFINITE;
            // Evaluate the position statically
            if (InCheck)
            {
                ss->static_eval = VALUE_NONE;
            }
            else
            {
                if (tt_hit)
                {
                    best_value = tte->eval ();
                    // Never assume anything on values stored in TT
                    if (VALUE_NONE == best_value)
                    {
                        best_value = evaluate (pos);
                    }
                    ss->static_eval = best_value;

                    // Can tt_value be used as a better position evaluation?
                    if (  VALUE_NONE != tt_value
                       && (tt_bound & (best_value < tt_value ? BOUND_LOWER : BOUND_UPPER))
                       )
                    {
                        best_value = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = best_value =
                        (ss-1)->current_move != MOVE_NULL ? evaluate (pos) : -(ss-1)->static_eval + 2*TEMPO;
                }

                if (alpha < best_value)
                {
                    // Stand pat. Return immediately if static value is at least beta
                    if (best_value >= beta)
                    {
                        if (!tt_hit)
                        {
                            tte->save (posi_key, MOVE_NONE, value_to_tt (best_value, ss->ply), ss->static_eval, DEPTH_NONE, BOUND_LOWER, TT.generation ());
                        }

                        assert (-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
                        return best_value;
                    }

                    assert (best_value < beta);
                    // Update alpha here! always alpha < beta
                    if (PVNode) alpha = best_value;
                }

                futility_base = best_value + VALUE_EG_PAWN/2; // QS Futility Margin
            }

            // Initialize a MovePicker object for the current position, and prepare
            // to search the moves. Because the depth is <= 0 here, only captures,
            // queen promotions and checks (only if depth >= DEPTH_QS_CHECKS) will
            // be generated.
            MovePicker mp (pos, HistoryStatistics, tt_move, depth, _ok ((ss-1)->current_move) ? dst_sq ((ss-1)->current_move) : SQ_NO);
            StateInfo si;
            if (ci == NULL) { cc = CheckInfo (pos); ci = &cc; }

            Move move;
            u08 legals = 0;
            // Loop through the moves until no moves remain or a beta cutoff occurs
            while ((move = mp.next_move<false> ()) != MOVE_NONE)
            {
                assert (_ok (move));
                
                bool gives_check = pos.gives_check (move, *ci);
                
                if (!MateSearch)
                {
                    // Futility pruning
                    if (  !InCheck
                       && !gives_check
                       && futility_base > -VALUE_KNOWN_WIN
                       && futility_base <= alpha
                       && !pos.advanced_pawn_push (move)
                       )
                    {
                        assert (mtype (move) != ENPASSANT); // Due to !pos.advanced_pawn_push()

                        Value futility_value = futility_base + PIECE_VALUE[EG][ptype (pos[dst_sq (move)])];

                        if (futility_value <= alpha)
                        {
                            best_value = max (futility_value, best_value);
                            continue;
                        }
                        // Prune moves with negative or zero SEE
                        if (pos.see (move) <= VALUE_ZERO)
                        {
                            best_value = max (futility_base, best_value);
                            continue;
                        }
                    }

                    // Don't search moves with negative SEE values
                    if (  mtype (move) != PROMOTE
                       && (  !InCheck
                          // Detect non-capture evasions that are candidate to be pruned (evasion_prunable)
                          || (  best_value > -VALUE_MATE_IN_MAX_DEPTH
                             && !pos.capture (move)
                             && !pos.can_castle (pos.active ())
                             )
                          )
                       && pos.see_sign (move) < VALUE_ZERO
                       )
                    {
                        continue;
                    }
                }

                // Speculative prefetch as early as possible
                prefetch (reinterpret_cast<char*> (TT.cluster_entry (pos.posi_move_key (move))));

                // Check for legality just before making the move
                if (!pos.legal (move, ci->pinneds)) continue;

                ++legals;

                ss->current_move = move;
                // Make and search the move
                pos.do_move (move, si, gives_check);

                prefetch (reinterpret_cast<char*> (thread->pawn_table[pos.pawn_key ()]));
                prefetch (reinterpret_cast<char*> (thread->matl_table[pos.matl_key ()]));

                Value value;
                
                value =
                    gives_check ?
                        -quien_search<NT, true > (pos, ss+1, -beta, -alpha, depth-DEPTH_ONE) :
                        -quien_search<NT, false> (pos, ss+1, -beta, -alpha, depth-DEPTH_ONE);

                bool next_legal = PVNode && (ss+1)->pv != NULL && (ss+1)->pv[0] != MOVE_NONE && pos.pseudo_legal ((ss+1)->pv[0]) && pos.legal ((ss+1)->pv[0]);

                // Undo the move
                pos.undo_move ();

                assert (-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Check for new best move
                if (best_value < value)
                {
                    best_value = value;

                    if (alpha < value)
                    {
                        best_move = move;

                        if (PVNode)
                        {
                            if (next_legal)
                            {
                                update_pv (ss->pv, best_move, (ss+1)->pv);
                            }
                            else
                            {
                                Move *mm = ss->pv;
                                *mm++ = best_move; *mm = MOVE_NONE;
                            }
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            tte->save (posi_key, best_move, value_to_tt (best_value, ss->ply), ss->static_eval, qs_depth, BOUND_LOWER, TT.generation ());

                            assert (-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
                            return best_value;
                        }

                        assert (value < beta);
                        // Update alpha here! always alpha < beta
                        if (PVNode) alpha = value;
                    }
                }
            }
            
            // All legal moves have been searched.
            // A special case: If in check and no legal moves were found, it is checkmate.
            if (InCheck && 0 == legals)
            {
                // Plies to mate from the root
                best_value = mated_in (ss->ply);
            }

            tte->save (posi_key, best_move, value_to_tt (best_value, ss->ply), ss->static_eval, qs_depth,
                PVNode && pv_alpha < best_value ? BOUND_EXACT : BOUND_UPPER, TT.generation ());

            assert (-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }

        template<NodeT NT, bool SPNode, bool EarlyPruning>
        // depth_search<>() is the main depth limited search function
        // for Root/PV/NonPV nodes also for normal/splitpoint nodes.
        // It calls itself recursively with decreasing (remaining) depth
        // until we run out of depth, and then drops into quien_search.
        // When called just after a splitpoint the search is simpler because
        // already probed the hash table, done a null move search, and searched
        // the first move before splitting, don't have to repeat all this work again.
        // Also don't need to store anything to the hash table here.
        // This is taken care of after return from the splitpoint.
        inline Value depth_search  (Position &pos, Stack *ss, Value alpha, Value beta, Depth depth, bool cut_node)
        {
            const bool RootNode = NT == Root;
            const bool   PVNode = NT == Root || NT == PV;

            assert (-VALUE_INFINITE <= alpha && alpha < beta && beta <= +VALUE_INFINITE);
            assert (PVNode || alpha == beta-1);
            assert (depth > DEPTH_ZERO);

            Key   posi_key;
            bool  tt_hit = false;
            TTEntry *tte = NULL;

            Move  move
                , tt_move     = MOVE_NONE
                , exclude_move= MOVE_NONE
                , best_move   = MOVE_NONE;

            Value tt_value    = VALUE_NONE
                , static_eval = VALUE_NONE
                , best_value  = -VALUE_INFINITE;

            Depth tt_depth    = DEPTH_NONE;
            Bound tt_bound    = BOUND_NONE;

            // Step 1. Initialize node
            Thread *thread = pos.thread ();
            bool  in_check = pos.checkers () != U64(0);

            SplitPoint *splitpoint = NULL;
            StateInfo si;
            CheckInfo cc, *ci = NULL;

            if (SPNode)
            {
                splitpoint  = ss->splitpoint;
                best_value  = splitpoint->best_value;
                best_move   = splitpoint->best_move;

                assert (splitpoint->best_value > -VALUE_INFINITE);
                assert (splitpoint->legals > 0);
            }
            else
            {
                ss->ply = (ss-1)->ply + 1;

                // Used to send 'seldepth' info to GUI
                if (PVNode && thread->max_ply < ss->ply)
                {
                    thread->max_ply = ss->ply;
                }

                if (!RootNode)
                {
                    // Step 2. Check end condition
                    // Check for aborted search, immediate draw or maximum ply reached
                    if (Signals.force_stop || pos.draw () || ss->ply >= MAX_DEPTH)
                    {
                        return ss->ply >= MAX_DEPTH && !in_check ? evaluate (pos) : DrawValue[pos.active ()];
                    }

                    // Step 3. Mate distance pruning. Even if mate at the next move our score
                    // would be at best mates_in(ss->ply+1), but if alpha is already bigger because
                    // a shorter mate was found upward in the tree then there is no need to search
                    // further, will never beat current alpha. Same logic but with reversed signs
                    // applies also in the opposite condition of being mated instead of giving mate,
                    // in this case return a fail-high score.
                    alpha = max (mated_in (ss->ply +0), alpha);
                    beta  = min (mates_in (ss->ply +1), beta);

                    if (alpha >= beta) return alpha;
                }

                assert (0 <= ss->ply && ss->ply < MAX_DEPTH);
                
                ss->current_move = (ss+1)->exclude_move = MOVE_NONE;
                fill ((ss+2)->killer_moves, (ss+2)->killer_moves + sizeof ((ss+2)->killer_moves)/sizeof (*((ss+2)->killer_moves)), MOVE_NONE);

                // Step 4. Transposition table lookup
                // Don't want the score of a partial search to overwrite a previous full search
                // TT value, so use a different position key in case of an excluded move.
                exclude_move = ss->exclude_move;
                posi_key = exclude_move == MOVE_NONE ?
                            pos.posi_key () :
                            pos.posi_key () ^ Zobrist::EXC_KEY;

                tte      = TT.probe (posi_key, tt_hit);
                ss->tt_move = tt_move = RootNode ? RootMoves[IndexPV].pv[0] :
                                        tt_hit ? tte->move () : MOVE_NONE;
                if (tt_hit)
                {
                    tt_value = value_of_tt (tte->value (), ss->ply);
                    tt_depth = tte->depth ();
                    tt_bound = tte->bound ();
                }

                // At non-PV nodes we check for a fail high/low. We don't probe at PV nodes
                if (  !PVNode
                   && tt_hit
                   && tt_value != VALUE_NONE // Only in case of TT access race
                   && tt_depth >= depth
                   && (tt_value >= beta ? (tt_bound & BOUND_LOWER) : (tt_bound & BOUND_UPPER))
                   )
                {
                    ss->current_move = tt_move; // Can be MOVE_NONE

                    // If tt_move is quiet, update history, killer moves, countermove and followupmove on TT hit
                    if (  !in_check
                       && tt_value >= beta
                       && tt_move != MOVE_NONE
                       && !pos.capture_or_promotion (tt_move)
                       )
                    {
                        update_stats (pos, ss, tt_move, depth, NULL, 0);
                    }

                    return tt_value;
                }

                // Step 5. Evaluate the position statically and update parent's gain statistics
                if (in_check)
                {
                    ss->static_eval = static_eval = VALUE_NONE;
                }
                else
                {
                    if (tt_hit)
                    {
                        static_eval = tte->eval ();
                        // Never assume anything on values stored in TT
                        if (VALUE_NONE == static_eval)
                        {
                            static_eval = evaluate (pos);
                        }
                        ss->static_eval = static_eval;

                        // Can tt_value be used as a better position evaluation?
                        if (  VALUE_NONE != tt_value
                           && (tt_bound & (static_eval < tt_value ? BOUND_LOWER : BOUND_UPPER))
                           )
                        {
                            static_eval = tt_value;
                        }
                    }
                    else
                    {
                        ss->static_eval = static_eval =
                            (ss-1)->current_move != MOVE_NULL ? evaluate (pos) : -(ss-1)->static_eval + 2*TEMPO;

                        tte->save (posi_key, MOVE_NONE, VALUE_NONE, ss->static_eval, DEPTH_NONE, BOUND_NONE, TT.generation ());
                    }

                    if (EarlyPruning)
                    {
                        move = (ss-1)->current_move;
                        // Updates Gain Statistics
                        if (  move != MOVE_NONE
                           && move != MOVE_NULL
                           && mtype (move) == NORMAL
                           && (ss-0)->static_eval != VALUE_NONE
                           && (ss-1)->static_eval != VALUE_NONE
                           && pos.capture_type () == NONE
                           )
                        {
                            GainStatistics.update (pos, move, -(ss-1)->static_eval - (ss-0)->static_eval);
                        }


                        // Step 6. Razoring sort of forward pruning where rather than skipping an entire subtree,
                        // you search it to a reduced depth, typically one less than normal depth.
                        if (  !PVNode && !MateSearch
                           && depth < RazorDepth
                           && static_eval + RazorMargins[depth] <= alpha
                           && tt_move == MOVE_NONE
                           && !pos.pawn_on_7thR (pos.active ())
                           )
                        {
                            if (  depth <= 1*DEPTH_ONE
                               && static_eval + RazorMargins[3*DEPTH_ONE] <= alpha
                               )
                            {
                                return quien_search<NonPV, false> (pos, ss, alpha, beta, DEPTH_ZERO);
                            }

                            Value reduced_alpha = max (alpha - RazorMargins[depth], -VALUE_INFINITE);

                            Value value = quien_search<NonPV, false> (pos, ss, reduced_alpha, reduced_alpha+1, DEPTH_ZERO);

                            if (value <= reduced_alpha)
                            {
                                return value;
                            }
                        }

                        // Step 7. Futility pruning: child node
                        // Betting that the opponent doesn't have a move that will reduce
                        // the score by more than FutilityMargins[depth] if do a null move.
                        if (  !RootNode && !MateSearch
                           && depth < FutilityMarginDepth
                           && abs (static_eval) < +VALUE_KNOWN_WIN // Do not return unproven wins
                           && pos.non_pawn_material (pos.active ()) > VALUE_ZERO
                           )
                        {
                            Value stand_pat = static_eval - FutilityMargins[depth];

                            if (stand_pat >= beta)
                            {
                                return stand_pat;
                            }
                        }

                        // Step 8. Null move search with verification search
                        if (  !PVNode && !MateSearch
                           && depth > 1*DEPTH_ONE
                           && static_eval >= beta
                           && pos.non_pawn_material (pos.active ()) > VALUE_ZERO
                           )
                        {
                            assert ((ss-1)->current_move != MOVE_NONE && (ss-1)->current_move != MOVE_NULL);
                            assert (exclude_move == MOVE_NONE);

                            ss->current_move = MOVE_NULL;
                            
                            // Null move dynamic reduction based on depth and static evaluation
                            Depth reduced_depth = depth - ((0x337 + 0x43 * depth) / 0x100 + min ((static_eval - beta)/VALUE_EG_PAWN, 3))*DEPTH_ONE;

                            // Do null move
                            pos.do_null_move (si);

                            // Speculative prefetch as early as possible
                            prefetch (reinterpret_cast<char*> (TT.cluster_entry (pos.posi_key ())));

                            // Null (zero) window (alpha, beta) = (beta-1, beta):
                            Value null_value =
                                reduced_depth < DEPTH_ONE ?
                                    -quien_search<NonPV, false>        (pos, ss+1, -beta, -beta+1, DEPTH_ZERO) :
                                    -depth_search<NonPV, false, false> (pos, ss+1, -beta, -beta+1, reduced_depth, !cut_node);

                            // Undo null move
                            pos.undo_null_move ();

                            if (null_value >= beta)
                            {
                                // Don't do verification search at low depths
                                if (depth < 8*DEPTH_ONE && abs (beta) < +VALUE_KNOWN_WIN)
                                {
                                    // Don't return unproven unproven mates
                                    return abs (null_value) < +VALUE_MATE_IN_MAX_DEPTH ? null_value : beta;
                                }

                                // Do verification search at high depths
                                Value value =
                                    reduced_depth < DEPTH_ONE ?
                                        quien_search<NonPV, false>        (pos, ss, beta-1, beta, DEPTH_ZERO) :
                                        depth_search<NonPV, false, false> (pos, ss, beta-1, beta, reduced_depth, false);

                                if (value >= beta)
                                {
                                    // Don't return unproven unproven mates
                                    return abs (null_value) < +VALUE_MATE_IN_MAX_DEPTH ? null_value : beta;
                                }
                            }
                        }
                        
                        // Step 9. Prob-Cut
                        // If have a very good capture (i.e. SEE > see[captured_piece_type])
                        // and a reduced search returns a value much above beta,
                        // can (almost) safely prune the previous move.
                        if (  !PVNode && !MateSearch
                           && depth > ProbCutDepth
                           && abs (beta) < +VALUE_MATE_IN_MAX_DEPTH
                           )
                        {
                            Depth reduced_depth = depth - ProbCutDepth; // Shallow Depth
                            Value extended_beta = min (beta + VALUE_MG_PAWN, +VALUE_INFINITE); // ProbCut Threshold

                            // Initialize a MovePicker object for the current position,
                            // and prepare to search the moves.
                            MovePicker mp (pos, HistoryStatistics, tt_move, pos.capture_type ());
                            if (ci == NULL) { cc = CheckInfo (pos); ci = &cc; }

                            while ((move = mp.next_move<false> ()) != MOVE_NONE)
                            {
                                // Speculative prefetch as early as possible
                                prefetch (reinterpret_cast<char*> (TT.cluster_entry (pos.posi_move_key (move))));

                                if (!pos.legal (move, ci->pinneds)) continue;

                                ss->current_move = move;
                                    
                                pos.do_move (move, si, pos.gives_check (move, *ci));

                                prefetch (reinterpret_cast<char*> (thread->pawn_table[pos.pawn_key ()]));
                                prefetch (reinterpret_cast<char*> (thread->matl_table[pos.matl_key ()]));

                                Value value = -depth_search<NonPV, false, true> (pos, ss+1, -extended_beta, -extended_beta+1, reduced_depth, !cut_node);

                                pos.undo_move ();

                                if (value >= extended_beta)
                                {
                                    return value;
                                }
                            }
                        }

                        // Step 10. Internal iterative deepening (skipped when in check)
                        if (  tt_move == MOVE_NONE
                           && depth > (PVNode ? 4*DEPTH_ONE : 7*DEPTH_ONE)          // IID Activation Depth
                           && (PVNode || ss->static_eval + VALUE_EG_PAWN >= beta) // IID Margin
                           )
                        {
                            Depth iid_depth = (2*(depth - 2*DEPTH_ONE) - (PVNode ? DEPTH_ZERO : depth/2))/2; // IID Reduced Depth

                            depth_search<PVNode ? PV : NonPV, false, false> (pos, ss, alpha, beta, iid_depth, true);

                            tte = TT.probe (posi_key, tt_hit);
                            if (tt_hit)
                            {
                                tt_move  = tte->move ();
                                tt_value = value_of_tt (tte->value (), ss->ply);
                                tt_depth = tte->depth ();
                                tt_bound = tte->bound ();
                            }
                        }
                    }
                }

            }

            // Splitpoint start
            // When in check and at SPNode search starts from here

            Value value = -VALUE_INFINITE;

            bool improving =
                   ((ss-2)->static_eval == VALUE_NONE)
                || ((ss-0)->static_eval == VALUE_NONE)
                || ((ss-0)->static_eval >= (ss-2)->static_eval);

            bool singular_ext_node =
                   !RootNode && !SPNode
                && exclude_move == MOVE_NONE // Recursive singular search is not allowed
                &&      tt_move != MOVE_NONE
                &&    depth >= (PVNode ? 6*DEPTH_ONE : 8*DEPTH_ONE)
                && tt_depth >= depth-3*DEPTH_ONE
                && abs (tt_value) < +VALUE_KNOWN_WIN
                && (tt_bound & BOUND_LOWER);

            point time;

            if (RootNode)
            {
                if (Threadpool.main () == thread)
                {
                    time = now () - SearchTime;
                    if (time > INFO_INTERVAL)
                    {
                        sync_cout
                            << "info"
                            << " depth " << depth/DEPTH_ONE
                            << " time "  << time
                            << sync_endl;
                    }
                }
            }

            Move * counter_moves = _ok ((ss-1)->current_move) ?  CounterMoveStats.moves (pos, dst_sq ((ss-1)->current_move)) : NULL
               , *followup_moves = _ok ((ss-2)->current_move) ? FollowupMoveStats.moves (pos, dst_sq ((ss-2)->current_move)) : NULL;

            MovePicker mp (pos, HistoryStatistics, tt_move, depth, counter_moves, followup_moves, ss);
            if (ci == NULL) { cc = CheckInfo (pos); ci = &cc; }

            u08   legals = 0
                , quiets = 0;

            Move  quiet_moves[MAX_QUIETS]
                , pv[MAX_DEPTH+1];
            
            // Step 11. Loop through moves
            // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs
            while ((move = mp.next_move<SPNode> ()) != MOVE_NONE)
            {
                assert (_ok (move));

                if (move == exclude_move) continue;
                
                // At root obey the "searchmoves" option and skip moves not listed in
                // RootMove list, as a consequence any illegal move is also skipped.
                // In MultiPV mode also skip PV moves which have been already searched.
                if (RootNode && count (RootMoves.begin () + IndexPV, RootMoves.end (), move) == 0) continue;

                bool move_legal = RootNode || pos.legal (move, ci->pinneds);

                if (SPNode)
                {
                    // Shared counter cannot be decremented later if move turns out to be illegal
                    if (!move_legal) continue;

                    legals = ++splitpoint->legals;
                    splitpoint->spinlock.release ();
                }
                else
                {
                    ++legals;
                }

                //u64 nodes = U64(0);

                if (RootNode)
                {
                    //nodes = pos.game_nodes ();

                    Signals.root_1stmove = (1 == legals);

                    if (Threadpool.main () == thread)
                    {
                        time = now () - SearchTime;
                        if (time > INFO_INTERVAL)
                        {
                            sync_cout
                                << "info"
                                //<< " depth "          << depth/DEPTH_ONE
                                << " currmovenumber " << setw (2) << u16(legals + IndexPV)
                                << " currmove "       << move_to_can (move, Chess960)
                                << " time "           << time
                                << sync_endl;
                        }
                    }
                }
                
                if (PVNode)
                {
                    (ss+1)->pv = NULL;
                }

                Depth ext = DEPTH_ZERO;

                bool capture_or_promotion = pos.capture_or_promotion (move);

                bool gives_check = pos.gives_check (move, *ci);

                // Step 12. Extend the move which seems dangerous like ...checks etc.
                if (gives_check && pos.see_sign (move) >= VALUE_ZERO)
                {
                    ext = DEPTH_ONE;
                }

                // Singular extension(SE) search.
                // We extend the TT move if its value is much better than its siblings.
                // If all moves but one fail low on a search of (alpha-s, beta-s),
                // and just one fails high on (alpha, beta), then that move is singular
                // and should be extended. To verify this do a reduced search on all the other moves
                // but the tt_move, if result is lower than tt_value minus a margin then extend tt_move.
                if (  move_legal
                   && singular_ext_node
                   && move == tt_move
                   && ext == DEPTH_ZERO
                   )
                {
                    Value bound = tt_value - 2*(depth/DEPTH_ONE);

                    ss->exclude_move = move;
                    value = depth_search<NonPV, false, false> (pos, ss, bound-1, bound, depth/2, cut_node);
                    ss->exclude_move = MOVE_NONE;

                    if (value < bound) ext = DEPTH_ONE;
                }

                // Update the current move (this must be done after singular extension search)
                Depth new_depth = depth - DEPTH_ONE + ext;

                // Step 13. Pruning at shallow depth
                if (  !RootNode && !MateSearch
                   && !capture_or_promotion
                   && !in_check
                   && best_value > -VALUE_MATE_IN_MAX_DEPTH
                       // Dangerous
                   && !(  gives_check
                       || mtype (move) != NORMAL 
                       || pos.advanced_pawn_push (move)
                       )
                    )
                {
                    // Move count based pruning
                    if (  depth <  FutilityMoveCountDepth
                       && legals >= FutilityMoveCounts[improving][depth]
                       )
                    {
                        if (SPNode) splitpoint->spinlock.acquire ();
                        continue;
                    }

                    // Value based pruning
                    Depth predicted_depth = new_depth - reduction_depths<PVNode> (improving, depth, legals);

                    // Futility pruning: parent node
                    if (predicted_depth < FutilityMarginDepth)
                    {
                        Value futility_value = ss->static_eval + FutilityMargins[predicted_depth]
                                             + GainStatistics[pos[org_sq (move)]][dst_sq (move)] + VALUE_EG_PAWN/2;

                        if (alpha >= futility_value)
                        {
                            best_value = max (futility_value, best_value);

                            if (SPNode)
                            {
                                splitpoint->spinlock.acquire ();
                                if (splitpoint->best_value < best_value) splitpoint->best_value = best_value;
                            }
                            continue;
                        }
                    }

                    // Prune moves with negative SEE at low depths
                    if (  predicted_depth < RazorDepth
                       && pos.see_sign (move) < VALUE_ZERO
                       )
                    {
                        if (SPNode) splitpoint->spinlock.acquire ();
                        continue;
                    }
                }

                // Speculative prefetch as early as possible
                prefetch (reinterpret_cast<char*> (TT.cluster_entry (pos.posi_move_key (move))));

                if (!SPNode)
                {
                    if (!RootNode && !move_legal)
                    {
                        --legals;
                        continue;
                    }

                    if (quiets < MAX_QUIETS && !capture_or_promotion)
                    {
                        quiet_moves[quiets++] = move;
                    }
                }

                ss->current_move = move;

                // Step 14. Make the move
                pos.do_move (move, si, gives_check);

                prefetch (reinterpret_cast<char*> (thread->pawn_table[pos.pawn_key ()]));
                prefetch (reinterpret_cast<char*> (thread->matl_table[pos.matl_key ()]));

                bool full_depth_search;

                // Step 15. Reduced depth search (LMR).
                // If the move fails high will be re-searched at full depth.
                if (  depth > 2*DEPTH_ONE
                   && !capture_or_promotion
                   && legals > 1
                   && move != ss->killer_moves[0]
                   && move != ss->killer_moves[1]
                   )
                {
                    Depth reduction_depth = reduction_depths<PVNode> (improving, depth, legals);
                    // Increase reduction
                    if (  (!PVNode && cut_node)
                       || HistoryStatistics[pos[dst_sq (move)]][dst_sq (move)] < VALUE_ZERO
                       )
                    {
                        reduction_depth += DEPTH_ONE;
                    }
                    // Decrease reduction for counter moves
                    if (  reduction_depth > DEPTH_ZERO
                       && counter_moves != NULL
                       && (move == counter_moves[0] || move == counter_moves[1])
                       )
                    {
                        reduction_depth -= DEPTH_ONE;
                    }
                    // Decrease reduction for moves that escape a capture
                    if (  reduction_depth > DEPTH_ZERO
                       && mtype (move) == NORMAL
                       && ptype (pos[dst_sq (move)]) != PAWN
                       && pos.see (mk_move<NORMAL> (dst_sq (move), org_sq (move))) < VALUE_ZERO // Reverse move
                       )
                    {
                        reduction_depth -= DEPTH_ONE;
                    }

                    Depth reduced_depth;
                        
                    if (SPNode) alpha = splitpoint->alpha;
                    reduced_depth = max (new_depth - reduction_depth, DEPTH_ONE);
                    // Search with reduced depth
                    value = -depth_search<NonPV, false, true> (pos, ss+1, -alpha-1, -alpha, reduced_depth, true);

                    // Re-search at intermediate depth if reduction is very high
                    if (alpha < value && reduction_depth >= 4*DEPTH_ONE)
                    {
                        reduced_depth = max (new_depth - reduction_depth/2, DEPTH_ONE);
                        // Search with reduced depth
                        value = -depth_search<NonPV, false, true> (pos, ss+1, -alpha-1, -alpha, reduced_depth, true);

                        // Re-search at intermediate depth if reduction is very high
                        if (alpha < value && reduction_depth >= 8*DEPTH_ONE)
                        {
                            reduced_depth = max (new_depth - reduction_depth/4, DEPTH_ONE);
                            // Search with reduced depth
                            value = -depth_search<NonPV, false, true> (pos, ss+1, -alpha-1, -alpha, reduced_depth, true);
                        }
                    }

                    full_depth_search = alpha < value && reduction_depth != DEPTH_ZERO;
                }
                else
                {
                    full_depth_search = !PVNode || legals > 1;
                }

                // Step 16. Full depth search, when LMR is skipped or fails high
                if (full_depth_search)
                {
                    if (SPNode) alpha = splitpoint->alpha;

                    value =
                        new_depth < DEPTH_ONE ?
                            gives_check ?
                                -quien_search<NonPV, true >   (pos, ss+1, -alpha-1, -alpha, DEPTH_ZERO) :
                                -quien_search<NonPV, false>   (pos, ss+1, -alpha-1, -alpha, DEPTH_ZERO) :
                            -depth_search<NonPV, false, true> (pos, ss+1, -alpha-1, -alpha, new_depth, !cut_node);
                }

                // Do a full PV search on:
                // - first move
                // - fail high move (search only if value < beta)
                // otherwise let the parent node fail low with
                // alpha >= value and to try another better move.
                if (PVNode && (1 == legals || (alpha < value && (RootNode || value < beta))))
                {
                    fill (pv, pv + sizeof (pv)/sizeof (*pv), MOVE_NONE);
                    (ss+1)->pv = pv;

                    value =
                        new_depth < DEPTH_ONE ?
                            gives_check ?
                                -quien_search<PV, true >   (pos, ss+1, -beta, -alpha, DEPTH_ZERO) :
                                -quien_search<PV, false>   (pos, ss+1, -beta, -alpha, DEPTH_ZERO) :
                            -depth_search<PV, false, true> (pos, ss+1, -beta, -alpha, new_depth, false);
                }
                
                bool next_legal = PVNode && !RootNode && (ss+1)->pv != NULL && (ss+1)->pv[0] != MOVE_NONE && pos.pseudo_legal ((ss+1)->pv[0]) && pos.legal ((ss+1)->pv[0]);

                // Step 17. Undo move
                pos.undo_move ();

                assert (-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Step 18. Check for new best move
                if (SPNode)
                {
                    splitpoint->spinlock.acquire ();
                    alpha      = splitpoint->alpha;
                    best_value = splitpoint->best_value;
                }
                
                // Finished searching the move. If a stop or a cutoff occurred,
                // the return value of the search cannot be trusted,
                // and return immediately without updating best move, PV and TT.
                if (Signals.force_stop || thread->cutoff_occurred ())
                {
                    return VALUE_ZERO;
                }

                if (RootNode)
                {
                    RootMove &rm = *find (RootMoves.begin (), RootMoves.end (), move);
                    // Remember searched nodes counts for this rootmove
                    //rm.nodes += pos.game_nodes () - nodes;

                    // 1st legal move or new best move ?
                    if (1 == legals || alpha < value)
                    {
                        rm.new_value = value;
                        rm.pv.resize (1);

                        assert ((ss+1)->pv != NULL);
                        for (Move *m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                        {
                            rm.pv.push_back (*m);
                        }

                        // Record how often the best move has been changed in each iteration.
                        // This information is used for time management:
                        // When the best move changes frequently, allocate some more time.
                        if (legals > 1)
                        {
                            RootMoves.best_move_change++;
                        }
                    }
                    else
                    {
                        // All other moves but the PV are set to the lowest value, this
                        // is not a problem when sorting becuase sort is stable and move
                        // position in the list is preserved, just the PV is pushed up.
                        rm.new_value = -VALUE_INFINITE;
                    }
                }

                if (best_value < value)
                {
                    best_value = SPNode ? splitpoint->best_value = value : value;

                    if (alpha < value)
                    {
                        best_move = SPNode ? splitpoint->best_move = move : move;

                        if (PVNode && !RootNode)
                        {    
                            if (next_legal)
                            {
                                update_pv (SPNode ? splitpoint->ss->pv : ss->pv, best_move, (ss+1)->pv);
                            }
                            else
                            {
                                Move *mm = SPNode ? splitpoint->ss->pv : ss->pv;
                                *mm++ = best_move; *mm = MOVE_NONE;
                            }
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            if (SPNode) splitpoint->cut_off = true;

                            break;
                        }

                        assert (value < beta);
                        // Update alpha here! always alpha < beta
                        if (PVNode) alpha = SPNode ? splitpoint->alpha = value : value;
                    }
                }

                // Step 19. Check for splitting the search (at non-splitpoint node)
                if (  !SPNode
                   && Threadpool.split_depth <= depth
                   && Threadpool.size () > 1
                   && thread->splitpoint_count < MAX_SPLITPOINTS_PER_THREAD
                   && (   thread->active_splitpoint == NULL
                      || !thread->active_splitpoint->slave_searching
                      || (  Threadpool.size () > MAX_SLAVES_PER_SPLITPOINT
                         && thread->active_splitpoint->slaves_mask.count () == MAX_SLAVES_PER_SPLITPOINT
                         )
                      )
                   )
                {
                    assert (-VALUE_INFINITE <= alpha && alpha >= best_value && alpha < beta && best_value <= beta && beta <= +VALUE_INFINITE);

                    thread->split (pos, ss, alpha, beta, best_value, best_move, depth, legals, mp, NT, cut_node);
                        
                    if (Signals.force_stop || thread->cutoff_occurred ())
                    {
                        return VALUE_ZERO;
                    }

                    if (best_value >= beta)
                    {
                        break;
                    }
                }
            }

            // Step 20. Check for checkmate and stalemate
            if (!SPNode)
            {
                // If all possible moves have been searched and if there are no legal moves,
                // If in a singular extension search then return a fail low score (alpha).
                // Otherwise it must be mate or stalemate, so return value accordingly.
                if (0 == legals)
                {
                    best_value = 
                        exclude_move != MOVE_NONE ?
                            alpha : in_check ?
                                mated_in (ss->ply) : DrawValue[pos.active ()];
                }
                else
                // Quiet best move: Update history, killer, counter & followup moves
                if (  !in_check
                   && best_value >= beta
                   && best_move != MOVE_NONE
                   && !pos.capture_or_promotion (best_move)
                   )
                {
                    update_stats (pos, ss, best_move, depth, quiet_moves, quiets-1);
                }

                tte->save (posi_key, best_move,
                    value_to_tt (best_value, ss->ply), ss->static_eval, depth,
                    best_value >= beta ? BOUND_LOWER :
                        PVNode && best_move != MOVE_NONE ? BOUND_EXACT : BOUND_UPPER,
                    TT.generation ());
            }

            assert (-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }

        Stack Stacks[MAX_DEPTH+4]; // To allow referencing (ss+2)
        // iter_deepening_search() is the main iterative deepening search function.
        // It calls search() repeatedly with increasing depth until:
        // - the allocated thinking time has been consumed,
        // - the user stops the search,
        // - the maximum search depth is reached.
        // Time management; with iterative deepining enabled you can specify how long
        // you want the computer to think rather than how deep you want it to think. 
        inline void iter_deepening_search ()
        {
            Stack *ss = Stacks+2; // To allow referencing (ss-2)
            memset (ss-2, 0x00, 5*sizeof (*ss));

            TT.refresh ();
            GainStatistics.clear ();
            HistoryStatistics.clear ();
            CounterMoveStats.clear ();
            FollowupMoveStats.clear ();

            u08 skill_pv = Skills.pv_size ();
            if (skill_pv != 0) Skills.clear ();

            // Do have to play with skill handicap?
            // In this case enable MultiPV search by skill pv size
            // that will use behind the scenes to get a set of possible moves.
            LimitPV = min (max (MultiPV, skill_pv), RootSize);

            Value best_value = VALUE_ZERO
                , bound_a    = -VALUE_INFINITE
                , bound_b    = +VALUE_INFINITE
                , window_a   = VALUE_ZERO
                , window_b   = VALUE_ZERO;

            Depth depth = DEPTH_ZERO;

            // Iterative deepening loop until target depth reached
            while (++depth < MAX_DEPTH && !Signals.force_stop && (0 == Limits.depth || depth <= Limits.depth))
            {
                // Age out PV variability metric
                RootMoves.best_move_change *= 0.5;

                // Save last iteration's scores before first PV line is searched and
                // all the move scores but the (new) PV are set to -VALUE_INFINITE.
                for (u08 i = 0; i < RootSize; ++i)
                {
                    RootMoves[i].old_value = RootMoves[i].new_value;
                }

                bool aspiration = depth > 4*DEPTH_ONE;
                point iteration_time;

                // MultiPV loop. Perform a full root search for each PV line
                for (IndexPV = 0; IndexPV < LimitPV; ++IndexPV)
                {
                    // Reset Aspiration window starting size
                    if (aspiration)
                    {
                        window_a =
                        window_b =
                            Value(depth <= 32*DEPTH_ONE ? 22 - (u16(depth)-1)/4 : 14); // Decreasing window

                        bound_a = max (RootMoves[IndexPV].old_value - window_a, -VALUE_INFINITE);
                        bound_b = min (RootMoves[IndexPV].old_value + window_b, +VALUE_INFINITE);
                    }

                    // Start with a small aspiration window and, in case of fail high/low,
                    // research with bigger window until not failing high/low anymore.
                    do
                    {
                        best_value = depth_search<Root, false, true> (RootPos, ss, bound_a, bound_b, depth, false);

                        // Bring to front the best move. It is critical that sorting is
                        // done with a stable algorithm because all the values but the first
                        // and eventually the new best one are set to -VALUE_INFINITE and
                        // want to keep the same order for all the moves but the new PV
                        // that goes to the front. Note that in case of MultiPV search
                        // the already searched PV lines are preserved.
                        stable_sort (RootMoves.begin () + IndexPV, RootMoves.end ());

                        // Write PV back to transposition table in case the relevant
                        // entries have been overwritten during the search.
                        for (i08 i = IndexPV; i >= 0; --i)
                        {
                            RootMoves[i].insert_pv_into_tt (RootPos);
                        }

                        iteration_time = now () - SearchTime;

                        // If search has been stopped break immediately.
                        // Sorting and writing PV back to TT is safe becuase
                        // RootMoves is still valid, although refers to previous iteration.
                        if (Signals.force_stop) break;

                        // When failing high/low give some update
                        // (without cluttering the UI) before to re-search.
                        if (  MultiPV == 1
                           && iteration_time > INFO_INTERVAL
                           && (bound_a >= best_value || best_value >= bound_b)
                           )
                        {
                            sync_cout << info_multipv (RootPos, depth, bound_a, bound_b, iteration_time) << sync_endl;
                        }

                        // In case of failing low/high increase aspiration window and re-search,
                        // otherwise exit the loop.
                        if (best_value <= bound_a)
                        {
                            bound_b   = (bound_a + bound_b) / 2;
                            bound_a   = max (best_value - window_a, -VALUE_INFINITE);
                            window_a *= 1.50;
                            Signals.root_failedlow = true;
                            Signals.ponderhit_stop = false;
                        }
                        else
                        if (best_value >= bound_b)
                        {
                            bound_a   = (bound_a + bound_b) / 2;
                            bound_b   = min (best_value + window_b, +VALUE_INFINITE);
                            window_b *= 1.50;
                        }
                        else break;

                        assert (-VALUE_INFINITE <= bound_a && bound_a < bound_b && bound_b <= +VALUE_INFINITE);
                    } while (true);

                    // Sort the PV lines searched so far and update the GUI
                    stable_sort (RootMoves.begin (), RootMoves.begin () + IndexPV + 1);

                    if (Signals.force_stop) break;

                    if (IndexPV + 1 == LimitPV || iteration_time > INFO_INTERVAL)
                    {
                        sync_cout << info_multipv (RootPos, depth, bound_a, bound_b, iteration_time) << sync_endl;
                    }
                }

                if (Signals.force_stop) break;

                if (ContemptValue != 0)
                {
                    Value valued_contempt = Value(i32(RootMoves[0].new_value)/ContemptValue);
                    DrawValue[ RootColor] = BaseContempt[ RootColor] - valued_contempt;
                    DrawValue[~RootColor] = BaseContempt[~RootColor] + valued_contempt;
                }

                // If skill levels are enabled and time is up, pick a sub-optimal best move
                if (skill_pv != 0 && Skills.can_pick_move (depth))
                {
                    Skills.play_move ();
                }

                iteration_time = now () - SearchTime;

                if (SearchLogWrite)
                {
                    LogFile logfile (SearchLog);
                    logfile << pretty_pv (RootPos, depth, RootMoves[0].new_value, iteration_time, &RootMoves[0].pv[0]) << endl;
                }

                // Stop the search early:
                bool stop = false;

                // Do have time for the next iteration? Can stop searching now?
                if (!Signals.ponderhit_stop && Limits.use_timemanager ())
                {
                    // Time adjustments
                    if (aspiration && LimitPV == 1)
                    {
                        // Take in account some extra time if the best move has changed
                        TimeMgr.instability (RootMoves.best_move_change);
                    }

                    // If there is only one legal move available or 
                    // If all of the available time has been used.
                    if (RootSize == 1 || iteration_time > TimeMgr.available_time ())
                    {
                        stop = true;
                    }
                }
                else
                {
                    // Have found a "mate in <x>"?
                    if (  MateSearch
                       && best_value >= +VALUE_MATE_IN_MAX_DEPTH
                       && i16(VALUE_MATE - best_value) <= 2*Limits.mate
                       )
                    {
                        stop = true;
                    }
                }

                if (stop)
                {
                    // If allowed to ponder do not stop the search now but
                    // keep pondering until GUI sends "ponderhit" or "stop".
                    Limits.ponder ? Signals.ponderhit_stop = true : Signals.force_stop = true;
                }

            }

            if (skill_pv != 0) Skills.play_move ();
        }

        // perft<>() is our utility to verify move generation. All the leaf nodes
        // up to the given depth are generated and counted and the sum returned.
        template<bool RootNode>
        inline u64 perft (Position &pos, Depth depth)
        {
            u64 leaf_nodes = U64(0);

            CheckInfo ci (pos);
            for (MoveList<LEGAL> ms (pos); *ms != MOVE_NONE; ++ms)
            {
                u64 inter_nodes;
                if (RootNode && depth <= 1*DEPTH_ONE)
                {
                    inter_nodes = 1;
                }
                else
                {
                    StateInfo si;
                    pos.do_move (*ms, si, pos.gives_check (*ms, ci));
                    inter_nodes = depth <= 2*DEPTH_ONE ?
                                    MoveList<LEGAL>(pos).size () : perft<false> (pos, depth-DEPTH_ONE);
                    pos.undo_move ();
                }

                if (RootNode)
                {
                    sync_cout <<  left << setw ( 7)
                              //<< move_to_can (*ms, Chess960)
                              << move_to_san (*ms, pos)
                              << right << setw (16)
                              << setfill ('.') << inter_nodes << setfill (' ')
                              << left << sync_endl;
                }

                leaf_nodes += inter_nodes;
            }

            return leaf_nodes;
        }

    }

    bool                Chess960        = false;

    LimitsT             Limits;
    SignalsT volatile   Signals;

    RootMoveList        RootMoves;
    Position            RootPos (0);
    StateInfoStackPtr   SetupStates;

    point               SearchTime;

    u08                 MultiPV         = 1;
    //i32                 MultiPV_cp      = 0;

    i16                 FixedContempt   = 0
        ,               ContemptTime    = 24
        ,               ContemptValue   = 34;

    string              HashFile        = "Hash.dat";
    u16                 AutoSaveHashTime= 0;
    bool                AutoLoadHash    = false;

    string              BookFile        = "";
    bool                BestBookMove    = true;
    PolyglotBook        Book;

    string              SearchLog       = "";

    Skill               Skills;

    // ------------------------------------

    // RootMove::insert_pv_in_tt() is called at the end of a search iteration, and
    // inserts the PV back into the TT. This makes sure the old PV moves are searched
    // first, even if the old TT entries have been overwritten.
    void   RootMove::insert_pv_into_tt (Position &pos)
    {
        StateInfo states[MAX_DEPTH], *si = states;

        size_t ply;
        for (ply = 0; ply < pv.size (); ++ply)
        {
            Move m = pv[ply];
            assert (MoveList<LEGAL> (pos).contains (m));

            bool  tt_hit = false;
            TTEntry *tte = TT.probe (pos.posi_key (), tt_hit);
            // Don't overwrite correct entries
            if (!tt_hit || tte->move () != m)
            {
                tte->save (pos.posi_key (), m, VALUE_NONE, VALUE_NONE, DEPTH_NONE, BOUND_NONE, TT.generation ());
            }

            pos.do_move (m, *si++);
        }

        while (0 != ply)
        {
            pos.undo_move ();
            --ply;
        }
    }
    
    // RootMove::extract_ponder_from_tt() is called in case we have no ponder move before
    // exiting the search, for instance in case we stop the search during a fail high at
    // root. We try hard to have a ponder move to return to the GUI, otherwise in case of
    // 'ponder on' we have nothing to think on.
    Move RootMove::extract_ponder_move_from_tt (Position &pos)
    {
        assert (pv.size () == 1);
        assert (pv[0] != MOVE_NONE);

        StateInfo st;
        pos.do_move (pv[0], st);

        bool  tt_hit = false;
        TTEntry *tte = TT.probe (pos.posi_key (), tt_hit);

        Move m = tt_hit && tte->move () != MOVE_NONE && MoveList<LEGAL> (pos).contains (tte->move ()) ?
                    tte->move () : MOVE_NONE;

        pos.undo_move ();

        pv.push_back (m);
        return m;
    }

    string RootMove::info_pv () const
    {
        stringstream ss;
        for (size_t i = 0; i < pv.size (); ++i)
        {
            ss << " " << move_to_can (pv[i], Chess960);
        }
        return ss.str ();
    }

    // ------------------------------------

    void RootMoveList::initialize (const Position &pos, const vector<Move> &root_moves)
    {
        best_move_change = 0.0;
        clear ();
        for (MoveList<LEGAL> ms (pos); *ms != MOVE_NONE; ++ms)
        {
            if (root_moves.empty () || count (root_moves.begin (), root_moves.end (), *ms))
            {
                push_back (RootMove (*ms));
            }
        }
    }

    // ------------------------------------

    u08  Skill::pv_size () const
    {
        return _level < MAX_SKILL_LEVEL ? min (MIN_SKILL_MULTIPV, RootSize) : 0;
    }

    // When playing with a strength handicap, choose best move among the first 'candidates'
    // RootMoves using a statistical rule dependent on 'level'. Idea by Heinz van Saanen.
    Move Skill::pick_move ()
    {
        static PRNG pr (Time::now ());

        _best_move = MOVE_NONE;

        u08   skill_pv   = pv_size ();
        // RootMoves are already sorted by score in descending order
        Value variance   = min (RootMoves[0].new_value - RootMoves[skill_pv - 1].new_value, VALUE_MG_PAWN);
        Value weakness   = Value(MAX_DEPTH - 2 * _level);
        Value best_value = -VALUE_INFINITE;
        // Choose best move. For each move score add two terms both dependent on
        // weakness, one deterministic and bigger for weaker moves, and one random,
        // then choose the move with the resulting highest score.
        for (u08 i = 0; i < skill_pv; ++i)
        {
            Value v = RootMoves[i].new_value
                    + weakness * i32(RootMoves[0].new_value - RootMoves[i].new_value)
                    + variance * i32(pr.rand<u32> () % weakness) * 2 / i32(VALUE_EG_PAWN);

            if (best_value < v)
            {
                best_value = v;
                _best_move = RootMoves[i].pv[0];
            }
        }
        return _best_move;
    }

    // Swap best PV line with the sub-optimal one
    void Skill::play_move ()
    {
        swap (RootMoves[0], *find (RootMoves.begin (), RootMoves.end (), _best_move != MOVE_NONE ? _best_move : pick_move ()));
    }

    // ------------------------------------

    u64  perft (Position &pos, Depth depth)
    {
        return perft<true> (pos, depth);
    }

    // Main searching method
    void think ()
    {
        RootColor   = RootPos.active ();
        RootPly     = RootPos.game_ply ();
        RootSize    = RootMoves.size ();

        MateSearch  = 0 != Limits.mate;

        SearchLogWrite = !white_spaces (SearchLog);
        if (SearchLogWrite)
        {
            LogFile logfile (SearchLog);

            logfile
                << "----------->\n" << boolalpha
                << "RootPos  : " << RootPos.fen ()                   << "\n"
                << "RootSize : " << u16(RootSize)                    << "\n"
                << "Infinite : " << Limits.infinite                  << "\n"
                << "Ponder   : " << Limits.ponder                    << "\n"
                << "ClockTime: " << Limits.game_clock[RootColor].time<< "\n"
                << "Increment: " << Limits.game_clock[RootColor].inc << "\n"
                << "MoveTime : " << Limits.movetime                  << "\n"
                << "MovesToGo: " << u16(Limits.movestogo)            << "\n"
                << " Depth Score    Time       Nodes  PV\n"
                << "-----------------------------------------------------------"
                << endl;
        }

        if (RootSize != 0)
        {
            if (!white_spaces (BookFile) && !Limits.infinite && !MateSearch)
            {
                trim (BookFile);
                convert_path (BookFile);

                if (!Book.is_open () && !white_spaces (BookFile))
                {
                    Book.open (BookFile, ios_base::in|ios_base::binary);
                }
                if (Book.is_open ())
                {
                    Move book_move = Book.probe_move (RootPos, BestBookMove);
                    if (book_move != MOVE_NONE && count (RootMoves.begin (), RootMoves.end (), book_move))
                    {
                        swap (RootMoves[0], *find (RootMoves.begin (), RootMoves.end (), book_move));
                        goto finish;
                    }
                }
            }

            TimeMgr.initialize (Limits.game_clock[RootColor], Limits.movestogo, RootPly);

            i16 timed_contempt = 0;
            i16 diff_time = 0;
            if (  ContemptTime != 0
               && (diff_time = i16(Limits.game_clock[RootColor].time - Limits.game_clock[~RootColor].time)/MILLI_SEC) != 0
               //&& ContemptTime <= abs (diff_time)
               )
            {
                timed_contempt = diff_time/ContemptTime;
            }

            Value contempt = cp_to_value (double(FixedContempt + timed_contempt) / 0x64);
            DrawValue[ RootColor] = BaseContempt[ RootColor] = VALUE_DRAW - contempt;
            DrawValue[~RootColor] = BaseContempt[~RootColor] = VALUE_DRAW + contempt;

            // Reset the threads, still sleeping: will wake up at split time
            for (size_t idx = 0; idx < Threadpool.size (); ++idx)
            {
                Threadpool[idx]->max_ply = 0;
            }

            if (AutoLoadHash)
            {
                TT.load (HashFile);
                AutoLoadHash = false;
            }
            if (AutoSaveHashTime != 0 && !white_spaces (HashFile))
            {
                FirstAutoSave = true;
                Threadpool.auto_save_th        = new_thread<TimerThread> ();
                Threadpool.auto_save_th->task  = auto_save;
                Threadpool.auto_save_th->resolution = AutoSaveHashTime*MINUTE_MILLI_SEC;
                Threadpool.auto_save_th->start ();
                Threadpool.auto_save_th->notify_one ();
            }

            Threadpool.check_limits_th->start ();
            Threadpool.check_limits_th->notify_one (); // Wake up the recurring timer

            iter_deepening_search (); // Let's start searching !

            Threadpool.check_limits_th->stop ();

            if (Threadpool.auto_save_th != NULL)
            {
                Threadpool.auto_save_th->stop ();
                delete_thread (Threadpool.auto_save_th);
                Threadpool.auto_save_th = NULL;
            }

            if (SearchLogWrite)
            {
                LogFile logfile (SearchLog);

                point time = now () - SearchTime;

                logfile
                    << "Time (ms)  : " << time                                      << "\n"
                    << "Nodes (N)  : " << RootPos.game_nodes ()                     << "\n"
                    << "Speed (N/s): " << RootPos.game_nodes ()*MILLI_SEC / max (time, point(1)) << "\n"
                    << "Hash-full  : " << TT.hash_full ()                           << "\n"
                    << "Best move  : " << move_to_san (RootMoves[0].pv[0], RootPos) << "\n";
                if (   RootMoves[0].pv[0] != MOVE_NONE
                   && (RootMoves[0].pv.size () > 1 || RootMoves[0].extract_ponder_move_from_tt (RootPos) != MOVE_NONE)
                   )
                {
                    StateInfo si;
                    RootPos.do_move (RootMoves[0].pv[0], si);
                    logfile << "Ponder move: " << move_to_san (RootMoves[0].pv[1], RootPos) << "\n";
                    RootPos.undo_move ();
                }
                logfile << endl;
            }
        }
        else
        {
            sync_cout
                << "info"
                << " depth " << 0
                << " score " << to_string (RootPos.checkers () != U64(0) ? -VALUE_MATE : VALUE_DRAW)
                << " time "  << 0
                << sync_endl;

            RootMoves.push_back (RootMove (MOVE_NONE));

            if (SearchLogWrite)
            {
                LogFile logfile (SearchLog);

                logfile
                    << pretty_pv (RootPos, 0, RootPos.checkers () != U64(0) ? -VALUE_MATE : VALUE_DRAW, 0, &RootMoves[0].pv[0]) << "\n"
                    << "Time (ms)  : " << 0        << "\n"
                    << "Nodes (N)  : " << 0        << "\n"
                    << "Speed (N/s): " << 0        << "\n"
                    << "Hash-full  : " << 0        << "\n"
                    << "Best move  : " << "(none)" << "\n"
                    << endl;
            }

        }

    finish:

        point time = now () - SearchTime;

        // When search is stopped this info is printed
        sync_cout
            << "info"
            << " time "     << time
            << " nodes "    << RootPos.game_nodes ()
            << " nps "      << RootPos.game_nodes () * MILLI_SEC / max (time, point(1));
        if (time > MILLI_SEC) cout << " hashfull " << TT.hash_full ();
        cout<< sync_endl;

        // When reach max depth arrive here even without Signals.force_stop is raised,
        // but if are pondering or in infinite search, according to UCI protocol,
        // shouldn't print the best move before the GUI sends a "stop" or "ponderhit" command.
        // Simply wait here until GUI sends one of those commands (that raise Signals.force_stop).
        if (!Signals.force_stop && (Limits.ponder || Limits.infinite))
        {
            Signals.ponderhit_stop = true;
            RootPos.thread ()->wait_for (Signals.force_stop);
        }

        assert (RootMoves[0].pv.size () != 0);

        // Best move could be MOVE_NONE when searching on a stalemate position
        sync_cout << "bestmove " << move_to_can (RootMoves[0].pv[0], Chess960);
        if (   RootMoves[0].pv[0] != MOVE_NONE
           && (RootMoves[0].pv.size () > 1 || RootMoves[0].extract_ponder_move_from_tt (RootPos) != MOVE_NONE)
           )
        {
            cout << " ponder " << move_to_can (RootMoves[0].pv[1], Chess960);
        }
        cout << sync_endl;

    }

    // initialize() is called during startup to initialize various lookup tables
    void initialize ()
    {
        u08 d;  // depth
        u08 mc; // move count
        // Initialize lookup tables
        for (d = 0; d < RazorDepth; ++d)
        {
            RazorMargins         [d] = Value(i32(0x200 + (0x20 + 0*d)*d));
        }
        for (d = 0; d < FutilityMarginDepth; ++d)
        {
            FutilityMargins      [d] = Value(i32(0x00 + (0xC8 + 0*d)*d));
        }
        for (d = 0; d < FutilityMoveCountDepth; ++d)
        {
            FutilityMoveCounts[0][d] = u08(2.40 + 0.773 * pow (0.00 + d, 1.80));
            FutilityMoveCounts[1][d] = u08(2.90 + 1.045 * pow (0.49 + d, 1.80));
        }

        double red[2];
        ReductionDepths[0][0][0][0] =
        ReductionDepths[0][1][0][0] =
        ReductionDepths[1][0][0][0] =
        ReductionDepths[1][1][0][0] = DEPTH_ZERO;
        // Initialize reductions lookup table
        for (d = 1; d < ReductionDepth; ++d) // depth
        {
            for (mc = 1; mc < ReductionMoveCount; ++mc) // move-count
            {
                red[0] = 0.000 + log (double(d)) * log (double(mc)) / 3.00;
                red[1] = 0.333 + log (double(d)) * log (double(mc)) / 2.25;
                ReductionDepths[1][1][d][mc] = red[0] >= 1.0f ? Depth (u08(red[0] + 0.5)) : DEPTH_ZERO;
                ReductionDepths[0][1][d][mc] = red[1] >= 1.0f ? Depth (u08(red[1] + 0.5)) : DEPTH_ZERO;

                ReductionDepths[1][0][d][mc] = ReductionDepths[1][1][d][mc];
                ReductionDepths[0][0][d][mc] = ReductionDepths[0][1][d][mc];
                // Increase reduction when eval is not improving
                if (ReductionDepths[0][0][d][mc] >= 2*DEPTH_ONE)
                {
                    ReductionDepths[0][0][d][mc] += DEPTH_ONE;
                }
            }
        }
    }

}

namespace Threads {

    // check_limits() is called by the timer thread when the timer triggers.
    // It is used to print debug info and, more importantly,
    // to detect when out of available time or reached limits
    // and thus stop the search.
    void check_limits ()
    {
        static point last_time = now ();

        point now_time = now ();
        if (now_time - last_time >= MILLI_SEC)
        {
            last_time = now_time;
            dbg_print ();
        }

        // An engine may not stop pondering until told so by the GUI
        if (Limits.ponder) return;

        if (Limits.use_timemanager ())
        {
            point movetime = now_time - SearchTime;
            if (  movetime > TimeMgr.maximum_time () - 2 * TIMER_RESOLUTION
                  // Still at first move
               || (   Signals.root_1stmove
                  && !Signals.root_failedlow
                  && movetime > TimeMgr.available_time () * 0.75f
                  )
               )
            {
               Signals.force_stop = true;
            }
        }
        else
        if (Limits.movetime != 0)
        {
            point movetime = now_time - SearchTime;
            if (movetime >= Limits.movetime)
            {
                Signals.force_stop = true;
            }
        }
        else
        if (Limits.nodes != 0)
        {
            u64 nodes = RootPos.game_nodes ();

            Threadpool.spinlock.acquire ();
            
            // Loop across all splitpoints and sum accumulated splitpoint nodes plus
            // all the currently active positions nodes.
            for (size_t idx1 = 0; idx1 < Threadpool.size (); ++idx1)
            {
                Thread *thread = Threadpool[idx1];
                for (size_t count = 0; count < thread->splitpoint_count; ++count)
                {
                    SplitPoint &sp = thread->splitpoints[count];
                    sp.spinlock.acquire ();

                    nodes += sp.nodes;
                    for (size_t idx2 = 0; idx2 < Threadpool.size (); ++idx2)
                    {
                        if (sp.slaves_mask.test (idx2))
                        {
                            Position *pos = Threadpool[idx2]->active_pos;
                            if (pos != NULL) nodes += pos->game_nodes ();
                        }
                    }

                    sp.spinlock.release ();
                }
            }

            Threadpool.spinlock.release ();

            if (nodes >= Limits.nodes)
            {
                Signals.force_stop = true;
            }
        }
    }

    void auto_save ()
    {
        if (FirstAutoSave)
        {
            FirstAutoSave = false;
            return;
        }
        TT.save (HashFile);
    }

    // Thread::idle_loop() is where the thread is parked when it has no work to do
    void Thread::idle_loop ()
    {
        // Pointer 'splitpoint' is not null only if called from split<>(), and not
        // at the thread creation. So it means this is the splitpoint's master.
        SplitPoint *splitpoint = active_splitpoint;
        assert (splitpoint == NULL || (splitpoint->master == this && searching));

        do
        {
            // If this thread has been assigned work, launch a search
            while (searching)
            {
                assert (alive);

                Threadpool.spinlock.acquire ();

                assert (active_splitpoint != NULL);
                SplitPoint *sp = active_splitpoint;

                Threadpool.spinlock.release ();

                Stack stack[MAX_DEPTH+4], *ss = stack+2;    // To allow referencing (ss+2) & (ss-2)
                Position pos (*(sp->pos), this);
                
                memcpy (ss-2, sp->ss-2, 5*sizeof (*ss));

                ss->splitpoint = sp;

                // Lock splitpoint
                sp->spinlock.acquire ();

                assert (active_pos == NULL);

                active_pos = &pos;

                switch (sp->node_type)
                {
                case  Root: depth_search<Root , true, true> (pos, ss, sp->alpha, sp->beta, sp->depth, sp->cut_node); break;
                case    PV: depth_search<PV   , true, true> (pos, ss, sp->alpha, sp->beta, sp->depth, sp->cut_node); break;
                case NonPV: depth_search<NonPV, true, true> (pos, ss, sp->alpha, sp->beta, sp->depth, sp->cut_node); break;
                default   : assert (false);
                }

                assert (searching);
                searching  = false;
                active_pos = NULL;
                sp->slaves_mask.reset (index);
                sp->slave_searching = false;
                sp->nodes += pos.game_nodes ();

                // Wake up master thread so to allow it to return from the idle loop
                // in case the last slave of the splitpoint.
                if (this != sp->master && sp->slaves_mask.none ())
                {
                    assert (!sp->master->searching);

                    sp->master->notify_one ();
                }

                // After releasing the lock, cannot access anymore any splitpoint
                // related data in a safe way becuase it could have been released under
                // our feet by the sp master.
                sp->spinlock.release ();

                // Try to late join to another split point if none of its slaves has already finished.
                SplitPoint *best_sp     = NULL;
                i32         min_level   = INT_MAX;

                for (size_t idx = 0; idx < Threadpool.size (); ++idx)
                {
                    Thread *thread = Threadpool[idx];
                    size_t  count  = thread->splitpoint_count; // Local copy

                    sp = count != 0 ? &thread->splitpoints[count-1] : NULL;

                    if (  sp != NULL
                       && sp->slave_searching
                       && sp->slaves_mask.count () < MAX_SLAVES_PER_SPLITPOINT
                       && available_to (sp->master)
                       )
                    {
                        assert (Threadpool.size () > 2);
                        assert (this != thread);
                        assert (splitpoint == NULL || !splitpoint->slaves_mask.none ());

                        // Prefer to join to SP with few parents to reduce the probability
                        // that a cut-off occurs above us, and hence we waste our work.
                        i32 level = 0;
                        for (SplitPoint *spp = thread->active_splitpoint; spp != NULL; spp = spp->parent_splitpoint)
                        {
                            ++level;
                        }

                        if (min_level > level)
                        {
                            best_sp   = sp;
                            min_level = level;
                        }
                    }
                }

                if (best_sp != NULL)
                {
                    // Recheck the conditions under lock protection
                    Threadpool.spinlock.acquire ();
                    best_sp->spinlock.acquire ();

                    if (  best_sp->slave_searching
                       && best_sp->slaves_mask.count () < MAX_SLAVES_PER_SPLITPOINT
                       && available_to (best_sp->master)
                       )
                    {
                        best_sp->slaves_mask.set (index);
                        active_splitpoint = best_sp;
                        searching = true;
                    }

                    best_sp->spinlock.release ();
                    Threadpool.spinlock.release ();
                }
            }

            // Avoid races with notify_one() fired from last slave of the splitpoint
            mutex.lock ();
            // If master and all slaves have finished then exit idle_loop()
            if (splitpoint != NULL && splitpoint->slaves_mask.none ())
            {
                assert (!searching);
                mutex.unlock ();
                break;
            }

            // If not searching, wait for a condition to be signaled instead of
            // wasting CPU time polling for work.
            // Do sleep after retesting sleep conditions under lock protection, in
            // particular to avoid a deadlock in case a master thread has,
            // in the meanwhile, allocated us and sent the notify_one() call before
            // the chance to grab the lock.
            if (alive && !searching)
            {
                sleep_condition.wait (mutex);
            }
            // Release the lock
            mutex.unlock ();

        } while (alive);
    }
}
