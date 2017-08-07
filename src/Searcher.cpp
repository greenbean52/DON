﻿#include "Searcher.h"

#include "Debugger.h"
#include "Evaluator.h"
#include "Notation.h"
#include "Polyglot.h"
#include "PRNG.h"
#include "TBsyzygy.h"
#include "Thread.h"
#include "Transposition.h"
#include "UCI.h"
#include "Zobrist.h"

using namespace std;
using namespace BitBoard;
using namespace Debugger;
using namespace Evaluator;
using namespace MoveGen;
using namespace Notation;
using namespace Polyglot;
using namespace TBSyzygy;
using namespace Threading;
using namespace Transposition;
using namespace UCI;
using namespace Zobrists;

// Extract ponder move from TT is called in case have no ponder move before exiting the search,
bool RootMove::extract_ponder_move_from_tt (Position &pos)
{
    assert(1 == size ());
    assert(MOVE_NONE != at (0));

    auto best_move = at (0);
    StateInfo si;
    pos.do_move (best_move, si);
    bool tt_hit;
    auto *tte = TT.probe (pos.si->posi_key, tt_hit);
    Move ponder_move;
    if (   tt_hit
        && MOVE_NONE != (ponder_move = tte->move ()) // Local copy to be SMP safe
        && pos.pseudo_legal (ponder_move)
        && pos.legal (ponder_move))
    {
        assert(MoveList<GenType::LEGAL> (pos).contains (ponder_move));
        *this += ponder_move;
    }
    pos.undo_move (best_move);
    return 1 < size ();
}

RootMove::operator string () const
{
    ostringstream oss;
    for (auto move : *this)
    {
        assert(MOVE_NONE != move);
        oss << " " << move_to_can (move);
    }
    return oss.str ();
}

RootMoves::operator std::string () const
{
    std::ostringstream oss;
    for (const auto &rm : *this)
    {
        oss << rm << "\n";
    }
    return oss.str ();
}

namespace {

    const i32 MaxValue = 1 << 28;

    enum Stage : u08
    {
        NATURAL_TT, CAPTURE_INIT, GOOD_CAPTURES, QUIET_INIT, QUIETS_1, QUIETS_2, BAD_CAPTURES,
        EVASION_TT, EVASION_INIT, EVASIONS,
        PROBCUT_CAPTURE_TT, PROBCUT_CAPTURE_INIT, PROBCUT_CAPTURES,
        QS_CHECK_TT, QS_CHECK_CAPTURE_INIT, QS_CHECK_CAPTURES, QS_CHECK_QUIET_INIT, QS_CHECK_QUIETS,
        QS_NO_CHECK_TT, QS_NO_CHECK_CAPTURE_INIT, QS_NO_CHECK_CAPTURES,
        QS_RECAPTURE_TT, QS_RECAPTURE_INIT, QS_RECAPTURES,
    };
}

// MovePicker class constructors. As arguments pass information to help
// it to return the (presumably) good moves first, to decide which moves to return
// (in the quiescence search, for instance, only want to search captures, promotions, and some checks)
// and about how important good move ordering is at the current node.

MovePicker::MovePicker (const Position &p, Move ttm, i16 d, const ButterflyHistory *bf, const PieceDestinyHistory **pd, const Move *km, Move cm)
    : pos (p)
    , tt_move (ttm)
    , threshold (Value(-4000 * d))
    , recap_sq (SQ_NO)
    , butterfly (bf)
    , piece_destiny (pd)
    , killers_moves (km, km + MaxKillers)
    , skip_quiets (false)
{
    assert(MOVE_NONE == tt_move
        || (pos.pseudo_legal (tt_move)
         && pos.legal (tt_move)));
    assert(d > 0);
    assert(threshold < VALUE_ZERO);

    if (0 == pos.si->checkers)
    {
        stage = Stage::NATURAL_TT;

        if (   MOVE_NONE != cm
            && tt_move != cm
            && std::find (killers_moves.begin (), killers_moves.end (), cm) == killers_moves.end ())
        {
            killers_moves.push_back (cm);
        }
        killers_moves.erase (std::remove_if (killers_moves.begin (),
                                             killers_moves.end (),
                                             [&](Move mm)
                                             {
                                                 return mm == MOVE_NONE
                                                     || mm == tt_move
                                                     ||  pos.capture (mm)
                                                     || !pos.pseudo_legal (mm)
                                                     || !pos.legal (mm);
                                             }),
                             killers_moves.end ());
    }
    else
    {
        stage = Stage::EVASION_TT;
    }
    
    if (MOVE_NONE == tt_move)
    {
        ++stage;
    }
}
MovePicker::MovePicker (const Position &p, Move ttm, i16 d, const ButterflyHistory *bf, const PieceDestinyHistory **pd, Square rs)
    : pos (p)
    , tt_move (ttm)
    , threshold (VALUE_ZERO)
    , recap_sq (SQ_NO)
    , butterfly (bf)
    , piece_destiny (pd)
    , skip_quiets (false)
{
    assert(MOVE_NONE == tt_move
        || (pos.pseudo_legal (tt_move)
         && pos.legal (tt_move)));
    assert(d <= 0);

    if (0 != pos.si->checkers)
    {
        stage = Stage::EVASION_TT;
    }
    else
    if (d > DepthQSNoCheck)
    {
        stage = Stage::QS_CHECK_TT;
    }
    else
    if (d > DepthQSRecapture)
    {
        stage = Stage::QS_NO_CHECK_TT;
    }
    else
    {
        stage = Stage::QS_RECAPTURE_TT;
        recap_sq = rs;
        if (   MOVE_NONE != tt_move
            && !(   pos.capture (tt_move)
                 && dst_sq (tt_move) == recap_sq))
        {
            tt_move = MOVE_NONE;
        }
    }
    if (MOVE_NONE == tt_move)
    {
        ++stage;
    }
}
MovePicker::MovePicker (const Position &p, Move ttm, Value thr)
    : pos (p)
    , tt_move (ttm)
    , threshold (thr)
    , recap_sq (SQ_NO)
    , butterfly (nullptr)
    , piece_destiny (nullptr)
    , skip_quiets (false)
{
    assert(0 == pos.si->checkers);
    assert(MOVE_NONE == tt_move
        || (pos.pseudo_legal (tt_move)
         && pos.legal (tt_move)));

    stage = Stage::PROBCUT_CAPTURE_TT;

    // In ProbCut we generate captures with SEE greater than or equal to the given threshold
    if (   MOVE_NONE != tt_move
        && !(   pos.capture (tt_move)
             && pos.see_ge (tt_move, threshold)))
    {
        tt_move = MOVE_NONE;
    }
    if (MOVE_NONE == tt_move)
    {
        ++stage;
    }
}

// Assigns a numerical move ordering score to each move in a move list.
// The moves with highest scores will be picked first.

// Winning and equal captures in the main search are ordered by MVV/LVA, preferring captures near our home rank.
// Surprisingly, this appears to perform slightly better than SEE-based move ordering,
// exchanging big pieces before capturing a hanging piece probably helps to reduce the subtree size.
// In the main search push captures with negative SEE values to the bad captures vector,
// but instead of doing it now we delay until the move has been picked up,
// saving some SEE calls in case of a cutoff.
template<> void MovePicker::value<GenType::CAPTURE> ()
{
    for (auto &vm : moves)
    {
        assert(pos.pseudo_legal (vm.move)
            && pos.legal (vm.move)
            && pos.capture_or_promotion (vm.move));

        vm.value =
              i32(PieceValues[MG][pos.cap_type (vm.move)])
            - 200 * rel_rank (pos.active, dst_sq (vm.move));
    }
}
template<> void MovePicker::value<GenType::QUIET> ()
{
    for (auto &vm : moves)
    {
        assert(pos.pseudo_legal (vm.move)
            && pos.legal (vm.move));

        auto mpc = pos[org_sq (vm.move)];
        assert(NO_PIECE != mpc);
        auto dst = dst_sq (vm.move);
        vm.value =
              (*butterfly)[pos.active][move_pp (vm.move)]
            + (*piece_destiny[0])[mpc][dst]
            + (*piece_destiny[1])[mpc][dst]
            + (*piece_destiny[3])[mpc][dst];
    }
}
// First captures ordered by MVV/LVA, then non-captures ordered by stats heuristics
template<> void MovePicker::value<GenType::EVASION> ()
{
    for (auto &vm : moves)
    {
        assert(pos.pseudo_legal (vm.move)
            && pos.legal (vm.move));
        
        if (pos.capture (vm.move))
        {
            vm.value =
                  i32(PieceValues[MG][pos.cap_type (vm.move)])
                - ptype (pos[org_sq (vm.move)])
                + MaxValue;
        }
        else
        {
            vm.value =
                  (*butterfly)[pos.active][move_pp (vm.move)];
        }
    }
}

// Finds the max move in the range [beg, end) and moves it to front.
// It is faster than sorting all the moves in advance when there are few moves.
const ValMove& MovePicker::next_max_move ()
{
    auto beg = moves.begin () + m++;
    auto max = std::max_element (beg, moves.end ());
    if (beg != max)
    {
        std::swap (*beg, *max);
    }
    return *beg;
}
// Returns a new legal move every time it is called, until there are no more moves left.
// It picks the move with the biggest value from a list of generated moves.
Move MovePicker::next_move ()
{
    START:
    switch (stage)
    {

    case Stage::NATURAL_TT:
    case Stage::EVASION_TT:
    case Stage::PROBCUT_CAPTURE_TT:
    case Stage::QS_CHECK_TT:
    case Stage::QS_NO_CHECK_TT:
    case Stage::QS_RECAPTURE_TT:
        ++stage;
        return tt_move;
        break;

    case Stage::CAPTURE_INIT:
        generate<GenType::CAPTURE> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::CAPTURE> ();
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::GOOD_CAPTURES:
        while (m < moves.size ())
        {
            auto move = next_max_move ().move;
            if (pos.see_ge (move))
            {
                return move;
            }
            // Losing capture, add it to the capture moves
            bad_capture_moves.push_back (move);
        }
        ++stage;
        /* fallthrough */
    case Stage::QUIET_INIT:
        generate<GenType::QUIET> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::QUIET> ();
        }
        // Killers to top of quiet move
        {
            i32 k = 0;
            for (auto km : killers_moves)
            {
                auto itr = std::find (moves.begin (), moves.end (), km);
                if (itr != moves.end ())
                {
                    itr->value = MaxValue - k++;
                }
            }
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::QUIETS_1:
        if (m < moves.size ())
        {
            auto beg = moves.begin () + m;
            auto max = std::max_element (beg, moves.end ());
            if (   !skip_quiets
                || max->value >= 0)
            {
                if (max->value < threshold)
                {
                    ++stage;
                    goto START;
                }
                if (beg != max)
                {
                    // TODO:: use std::rotate
                    auto tmp = *max;
                    for (; max != beg; --max)
                    {
                        *max = *(max - 1);
                    }
                    *max = tmp;
                }
                return ++m, beg->move;
            }
        }
        stage += 2;
        m = 0;
        goto START;
    case Stage::QUIETS_2:
        if (   m < moves.size ()
            && !skip_quiets)
        {
            return moves[m++].move;
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::BAD_CAPTURES:
        if (m < bad_capture_moves.size ())
        {
            return bad_capture_moves[m++];
        }
        break;

    case Stage::EVASION_INIT:
        assert(0 != pos.si->checkers);
        generate<GenType::EVASION> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::EVASION> ();
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::EVASIONS:
        if (m < moves.size ())
        {
            return next_max_move ().move;
        }
        break;

    case Stage::PROBCUT_CAPTURE_INIT:
        generate<GenType::CAPTURE> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::CAPTURE> ();
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::PROBCUT_CAPTURES:
        while (m < moves.size ())
        {
            auto move = next_max_move ().move;
            if (pos.see_ge (move, threshold))
            {
                return move;
            }
        }
        break;

    case Stage::QS_CHECK_CAPTURE_INIT:
        generate<GenType::CAPTURE> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::CAPTURE> ();
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::QS_CHECK_CAPTURES:
        if (m < moves.size ())
        {
            return next_max_move ().move;
        }
        ++stage;
        /* fallthrough */
    case Stage::QS_CHECK_QUIET_INIT:
        generate<GenType::QUIET_CHECK> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::QS_CHECK_QUIETS:
        if (m < moves.size ())
        {
            return moves[m++].move;
        }
        break;

    case Stage::QS_NO_CHECK_CAPTURE_INIT:
        generate<GenType::CAPTURE> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::CAPTURE> ();
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::QS_NO_CHECK_CAPTURES:
        if (m < moves.size ())
        {
            return next_max_move ().move;
        }
        break;

    case Stage::QS_RECAPTURE_INIT:
        generate<GenType::CAPTURE> (moves, pos);
        filter_illegal (moves, pos);
        if (MOVE_NONE != tt_move)
        {
            moves.erase (std::remove (moves.begin (), moves.end (), tt_move), moves.end ());
        }
        if (1 < moves.size ())
        {
            value<GenType::CAPTURE> ();
        }
        ++stage;
        m = 0;
        /* fallthrough */
    case Stage::QS_RECAPTURES:
        assert(SQ_NO != recap_sq);
        while (m < moves.size ())
        {
            auto move = next_max_move ().move;
            if (dst_sq (move) == recap_sq)
            {
                return move;
            }
        }
        break;

    default:
        assert(false);
        break;
    }
    return MOVE_NONE;
}

namespace Searcher {

    Limit  Limits;

    u08    MultiPV =        1;
    //i32    MultiPV_cp =     0;

    i16    FixedContempt =  0
        ,  ContemptTime =   30
        ,  ContemptValue =  50;

    string HashFile =       "Hash.dat";
    bool   RetainHash =     false;

    bool   OwnBook =        false;
    string BookFile =       "Book.bin";
    bool   BookMoveBest =   true;
    i16    BookUptoMove =   20;

    i16    TBProbeDepth =   1;
    i32    TBLimitPiece =   6;
    bool   TBUseRule50 =    true;
    bool   TBHasRoot =      false;
    Value  TBValue =        VALUE_ZERO;

    string OutputFile =     Empty;

    namespace {

        const i16 MaxRazorDepth = 4;
        // RazorMargins[depth]
        Value RazorMargins[MaxRazorDepth] = { Value(0), Value(570), Value(602), Value(554) };

        const i16 MaxFutilityDepth = 16;
        // FutilityMoveCounts[improving][depth]
        u08 FutilityMoveCounts[2][MaxFutilityDepth];

        const i16 MaxReductionDepth = 64;
        const u08 MaxReductionMoveCount = 64;
        // ReductionDepths[pv][improving][depth][move_count]
        i16 ReductionDepths[2][2][MaxReductionDepth][MaxReductionMoveCount];
        i16 reduction_depth (bool pv, bool imp, i16 d, u08 mc)
        {
            return ReductionDepths[pv ? 1 : 0]
                                  [imp ? 1 : 0]
                                  [std::min (d, i16(MaxReductionDepth-1))]
                                  [std::min (mc, u08(MaxReductionMoveCount-1))];
        }

        Value DrawValue[CLR_NO]
            , BaseContempt[CLR_NO];

        ofstream OutputStream;

        // Stats bonus, based on depth
        i32 stat_bonus (i16 depth)
        {
            return depth <= 17 ? depth*(depth + 2) - 2 : 0;
        }

        // Updates countermoves and followupmoves history stats
        void update_continuation_histories (Stack *const &ss, Piece pc, Square dst, i32 value)
        {
            for (auto i : {1, 2, 4})
            {
                if (_ok ((ss-i)->played_move))
                {
                    (ss-i)->piece_destiny->update (pc, dst, value);
                }
            }
        }
        // Updates move sorting heuristics
        void update_histories (Stack *const &ss, const Position &pos, Move move, i32 value)
        {
            if (ss->killer_moves[0] != move)
            {
                ss->killer_moves[1] = ss->killer_moves[0];
                ss->killer_moves[0] = move;
            }
            assert(1 == std::count (ss->killer_moves, ss->killer_moves + MaxKillers, move));

            auto m = (ss-1)->played_move;
            if (_ok (m))
            {
                pos.thread->counter_moves[pos[fix_dst_sq (m)]][dst_sq (m)] = move;
            }

            pos.thread->butterfly.update (pos.active, move, value);
            update_continuation_histories (ss, pos[org_sq (move)], dst_sq (move), value);
        }

        // Appends the move and child pv
        void update_pv (Moves &pv, Move move, const Moves &child_pv)
        {
            pv.clear ();
            pv.push_back (move);
            for (auto m : child_pv)
            {
                pv.push_back (m);
            }
        }

        // It adjusts a mate score from "plies to mate from the root" to
        // "plies to mate from the current position". Non-mate scores are unchanged.
        // The function is called before storing a value to the transposition table.
        Value value_to_tt (Value v, i32 ply)
        {
            assert(VALUE_NONE != v);
            return v >= +VALUE_MATE_MAX_PLY ? v + ply :
                   v <= -VALUE_MATE_MAX_PLY ? v - ply :
                   v;
        }
        // It adjusts a mate score from "plies to mate from the current position" to "plies to mate from the root".
        // Non-mate scores are unchanged.
        // The function is called after retrieving a value of the transposition table.
        Value value_of_tt (Value v, i32 ply)
        {
            return v ==  VALUE_NONE         ? VALUE_NONE :
                   v >= +VALUE_MATE_MAX_PLY ? v - ply :
                   v <= -VALUE_MATE_MAX_PLY ? v + ply :
                   v;
        }

        // Formats PV information according to UCI protocol.
        // UCI requires that all (if any) unsearched PV lines are sent using a previous search score.
        string multipv_info (Thread *const &th, i16 depth, Value alfa, Value beta)
        {
            auto elapsed_time = std::max (Threadpool.main_thread ()->time_mgr.elapsed_time (), 1ULL);
            
            const auto &root_moves = th->root_moves;

            auto total_nodes = Threadpool.nodes ();
            auto tb_hits = Threadpool.tb_hits () + (TBHasRoot ? root_moves.size () : 0);

            ostringstream oss;
            for (u08 i = 0; i < Threadpool.pv_limit; ++i)
            {
                bool updated = 
                       i <= th->pv_index
                    && -VALUE_INFINITE != root_moves[i].new_value;

                i16 d =
                    updated ?
                        depth :
                        depth - 1;
                if (d <= 0)
                {
                    continue;
                }

                auto v =
                    updated ?
                        root_moves[i].new_value :
                        root_moves[i].old_value;
                bool tb =
                       TBHasRoot
                    && abs (v) < +VALUE_MATE - i32(MaxPlies);

                oss << "info"
                    << " multipv " << i + 1
                    << " depth " << d
                    << " seldepth " << root_moves[i].sel_depth
                    << " score " << to_string (tb ? TBValue : v)
                    << (   !tb
                        && i == th->pv_index ?
                            beta <= v ? " lowerbound" :
                                v <= alfa ? " upperbound" : "" : "")
                    << " nodes " << total_nodes
                    << " time " << elapsed_time
                    << " nps " << total_nodes * 1000 / elapsed_time
                    << " tbhits " << tb_hits;
                if (elapsed_time > 1000)
                {
                    oss << " hashfull " << TT.hash_full ();
                }
                oss << " pv" << root_moves[i];
                if (i+1 < Threadpool.pv_limit)
                {
                    oss << "\n";
                }
            }
            return oss.str ();
        }

        // The quiescence search function, which is called by the main depth limited search function
        // when the remaining depth is less than equal to 0.
        template<bool PVNode>
        Value quien_search (Position &pos, Stack *const &ss, Value alfa, Value beta, i16 depth = 0)
        {
            assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            assert(PVNode || (alfa == beta-1));
            assert(0 >= depth);
            assert(ss->ply > 1
                && ss->ply == (ss-1)->ply + 1
                && ss->ply <= MaxPlies);

            Value old_alfa;

            if (PVNode)
            {
                old_alfa = alfa; // To flag BOUND_EXACT when eval above alpha and no available moves
                ss->pv.clear ();
            }

            ss->played_move = MOVE_NONE;

            bool in_check = 0 != pos.si->checkers;
            // Check for an immediate draw or maximum ply reached.
            if (   ss->ply >= MaxPlies
                || pos.draw (ss->ply))
            {
                return ss->ply >= MaxPlies
                    && !in_check ?
                        evaluate (pos) :
                        DrawValue[pos.active];
            }

            Move move;
            // Transposition table lookup.
            Key  key = pos.si->posi_key;
            bool tt_hit;
            auto *tte = TT.probe (key, tt_hit);
            auto tt_move =
                tt_hit
             && MOVE_NONE != (move = tte->move ())
             && pos.pseudo_legal (move)
             && pos.legal (move) ?
                    move :
                    MOVE_NONE;
            assert(MOVE_NONE == tt_move
                || (pos.pseudo_legal (tt_move)
                 && pos.legal (tt_move)));
            auto tt_value =
                tt_hit ?
                    value_of_tt (tte->value (), ss->ply) :
                    VALUE_NONE;

            auto last_move = (ss-1)->played_move;

            // Decide whether or not to include checks.
            // Fixes also the type of TT entry depth that are going to use.
            // Note that in quien_search use only 2 types of depth: DepthQSCheck or DepthQSNoCheck.
            i16 qs_depth = in_check || DepthQSCheck <= depth ?
                            DepthQSCheck :
                            DepthQSNoCheck;

            if (   !PVNode
                && tt_hit
                && VALUE_NONE != tt_value // Only in case of TT access race
                && tte->depth () >= qs_depth
                && BOUND_NONE != (tte->bound () & (tt_value >= beta ?
                                                    BOUND_LOWER :
                                                    BOUND_UPPER)))
            {
                return tt_value;
            }

            Value best_value
                , futility_base;

            // Evaluate the position statically.
            if (in_check)
            {
                ss->static_eval = VALUE_NONE;
                // Starting from the worst case which is checkmate
                best_value =
                futility_base = mated_in (ss->ply);
            }
            else
            {
                Value tt_eval;
                if (tt_hit)
                {
                    // Never assume anything on values stored in TT.
                    ss->static_eval = tt_eval =
                        VALUE_NONE != tte->eval () ?
                            tte->eval () :
                            evaluate (pos);
                    // Can tt_value be used as a better position evaluation?
                    if (   VALUE_NONE != tt_value
                        && BOUND_NONE != (tte->bound () & (tt_value > tt_eval ?
                                                            BOUND_LOWER :
                                                            BOUND_UPPER)))
                    {
                        tt_eval = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = tt_eval =
                        MOVE_NULL != last_move ?
                            evaluate (pos) :
                            -(ss-1)->static_eval + Tempo*2;
                }

                if (alfa < tt_eval)
                {
                    // Stand pat. Return immediately if static value is at least beta.
                    if (tt_eval >= beta)
                    {
                        if (!tt_hit)
                        {
                            tte->save (key,
                                       MOVE_NONE,
                                       value_to_tt (tt_eval, ss->ply),
                                       ss->static_eval,
                                       DepthNone,
                                       BOUND_LOWER);
                        }

                        assert(-VALUE_INFINITE < tt_eval && tt_eval < +VALUE_INFINITE);
                        return tt_eval;
                    }

                    assert(tt_eval < beta);
                    // Update alfa! Always alfa < beta
                    if (PVNode)
                    {
                        alfa = tt_eval;
                    }
                }

                best_value = tt_eval;
                futility_base = best_value + 128;
            }

            auto best_move = MOVE_NONE;

            u08 move_count = 0;
            StateInfo si;

            const PieceDestinyHistory* piece_destiny[] = { nullptr, nullptr, nullptr, nullptr };
            // Initialize move picker (2) for the current position.
            MovePicker move_picker (pos, tt_move, depth, &pos.thread->butterfly, piece_destiny, dst_sq (last_move));
            // Loop through the moves until no moves remain or a beta cutoff occurs.
            while (MOVE_NONE != (move = move_picker.next_move ()))
            {
                assert(pos.pseudo_legal (move)
                    && pos.legal (move));

                ++move_count;
                auto mpc = pos[org_sq (move)];
                assert(NO_PIECE != mpc);
                bool gives_check = pos.gives_check (move);

                // Futility pruning
                if (   !in_check
                    && futility_base <= alfa
                    && futility_base > -VALUE_KNOWN_WIN
                    && !gives_check
                    //&& 0 == Limits.mate
                        // Advance pawn push
                    && !(   PAWN == ptype (mpc)
                         && rel_rank (pos.active, org_sq (move)) > R_4))
                {
                    // Futility pruning parent node
                    auto futility_value = futility_base + PieceValues[EG][ptype (pos[dst_sq (move)])];
                    if (futility_value <= alfa)
                    {
                        if (best_value < futility_value)
                        {
                            best_value = futility_value;
                        }
                        continue;
                    }
                    // Prune moves with negative or zero SEE
                    if (!pos.see_ge (move, VALUE_ONE))
                    {
                        if (best_value < futility_base)
                        {
                            best_value = futility_base;
                        }
                        continue;
                    }
                }

                // Don't search moves with negative SEE values
                if (   (   !in_check
                        // Evasion Prunable: Detect non-capture evasions that are candidate to be pruned
                        || (   (   0 != depth
                                || 2 < move_count)
                            && best_value > -VALUE_MATE_MAX_PLY
                            && !pos.capture (move)))
                    && PROMOTE != mtype (move)
                    //&& 0 == Limits.mate
                    && !pos.see_ge (move))
                {
                    continue;
                }

                // Speculative prefetch as early as possible
                prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                ss->played_move = move;

                // Make the move
                pos.do_move (move, si, gives_check);

                auto value = -quien_search<PVNode> (pos, ss+1, -beta, -alfa, depth - 1);

                // Undo the move
                pos.undo_move (move);

                assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Check for new best move
                if (best_value < value)
                {
                    best_value = value;

                    if (alfa < value)
                    {
                        // Update pv even in fail-high case
                        if (PVNode)
                        {
                            update_pv (ss->pv, move, (ss+1)->pv);
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            tte->save (key,
                                       move,
                                       value_to_tt (value, ss->ply),
                                       ss->static_eval,
                                       qs_depth,
                                       BOUND_LOWER);

                            assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);
                            return value;
                        }
                        assert(value < beta);

                        // Update alfa! Always alfa < beta
                        if (PVNode)
                        {
                            alfa = value;
                            best_move = move;
                        }
                    }
                }
            }
            
            tte->save (key,
                       best_move,
                       value_to_tt (best_value, ss->ply),
                       ss->static_eval,
                       qs_depth,
                          PVNode
                       && best_value > old_alfa ?
                           BOUND_EXACT :
                           BOUND_UPPER);

            assert(-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }
        // The main depth limited search function for both PV and non-PV nodes.
        template<bool PVNode>
        Value depth_search (Position &pos, Stack *const &ss, Value alfa, Value beta, i16 depth, bool cut_node, bool prun_node)
        {
            assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            assert(PVNode || (alfa == beta-1));
            assert(!(PVNode && cut_node));
            assert(0 < depth && depth < MaxPlies);
            assert(ss->ply >= 1
                && ss->ply == (ss-1)->ply + 1
                && ss->ply <= MaxPlies);

            // Step 1. Initialize node.
            ss->statistics = 0;
            ss->move_count = 0;
            
            if (PVNode)
            {
                if (pos.thread->sel_depth < ss->ply)
                {
                    pos.thread->sel_depth = ss->ply;
                }
            }

            // Check for the available remaining limit.
            if (Threadpool.main_thread () == pos.thread)
            {
                Threadpool.main_thread ()->check_limits ();
            }

            bool root_node = 1 == ss->ply;
            bool in_check = 0 != pos.si->checkers;

            if (!root_node)
            {
                // Step 2. Check for aborted search, immediate draw or maximum ply reached.
                if (   Threadpool.stop.load (std::memory_order::memory_order_relaxed)
                    || ss->ply >= MaxPlies
                    || pos.draw (ss->ply))
                {
                    return ss->ply >= MaxPlies
                        && !in_check ?
                            evaluate (pos) :
                            DrawValue[pos.active];
                }

                // Step 3. Mate distance pruning.
                // Even if mate at the next move our score would be at best mates_in(ss->ply+1),
                // but if alfa is already bigger because a shorter mate was found upward in the tree
                // then there is no need to search further, will never beat current alfa.
                // Same logic but with reversed signs applies also in the opposite condition of
                // being mated instead of giving mate, in this case return a fail-high score.
                alfa = std::max (mated_in (ss->ply+0), alfa);
                beta = std::min (mates_in (ss->ply+1), beta);
                if (alfa >= beta)
                {
                    return alfa;
                }
            }

            ss->played_move = MOVE_NONE;
            ss->piece_destiny = &pos.thread->continuation[NO_PIECE][0];
            assert(MOVE_NONE == (ss+1)->excluded_move);
            std::fill_n ((ss+2)->killer_moves, MaxKillers, MOVE_NONE);

            Move move;
            // Step 4. Transposition table lookup
            // Don't want the score of a partial search to overwrite a previous full search
            // TT value, so use a different position key in case of an excluded move.
            Key  key = pos.si->posi_key ^ Key(ss->excluded_move);
            bool tt_hit;
            auto *tte = TT.probe (key, tt_hit);
            auto tt_move =
                root_node ?
                    pos.thread->root_moves[pos.thread->pv_index][0] :
                    tt_hit
                 && MOVE_NONE != (move = tte->move ())
                 && pos.pseudo_legal (move)
                 && pos.legal (move) ?
                        move :
                        MOVE_NONE;
            assert(MOVE_NONE == tt_move
                || (pos.pseudo_legal (tt_move)
                 && pos.legal (tt_move)));

            auto tt_value =
                tt_hit ?
                    value_of_tt (tte->value (), ss->ply) :
                    VALUE_NONE;

            auto last_move = (ss-1)->played_move;

            // At non-PV nodes we check for an early TT cutoff.
            if (   !PVNode
                && tt_hit
                && VALUE_NONE != tt_value // Only in case of TT access race.
                && tte->depth () >= depth
                && BOUND_NONE != (tte->bound () & (tt_value >= beta ?
                                                    BOUND_LOWER :
                                                    BOUND_UPPER)))
            {
                // Update move sorting heuristics on tt_move.
                if (MOVE_NONE != tt_move)
                {
                    if (tt_value >= beta)
                    {
                        // Bonus for a quiet tt_move that fails high.
                        if (!pos.capture_or_promotion (tt_move))
                        {
                            update_histories (ss, pos, tt_move, stat_bonus (depth));
                        }
                        // Extra penalty for a quiet tt_move in previous ply when it gets refuted.
                        if (   1 == (ss-1)->move_count
                            && _ok (last_move)
                            && NONE == pos.si->capture
                            && !pos.si->promotion)
                        {
                            update_continuation_histories (ss-1, pos[fix_dst_sq (last_move)], dst_sq (last_move), -stat_bonus (depth + 1));
                        }
                    }
                    else
                    {
                        // Penalty for a quiet tt_move that fails low.
                        if (!pos.capture_or_promotion (tt_move))
                        {
                            auto penalty = -stat_bonus (depth);
                            pos.thread->butterfly.update (pos.active, tt_move, penalty);
                            update_continuation_histories (ss, pos[org_sq (tt_move)], dst_sq (tt_move), penalty);
                        }
                    }
                }
                return tt_value;
            }

            // Step 4A. Tablebase probe
            if (   !root_node
                && 0 != TBLimitPiece)
            {
                auto piece_count = pos.count<NONE> ();

                if (   (   piece_count < TBLimitPiece
                        || (   piece_count == TBLimitPiece
                            && depth >= TBProbeDepth))
                    && 0 == pos.si->clock_ply
                    && !pos.has_castleright (CR_ANY))
                {
                    ProbeState state;
                    WDLScore wdl = probe_wdl (pos, state);

                    if (ProbeState::FAILURE != state)
                    {
                        pos.thread->tb_hits.fetch_add (1, std::memory_order::memory_order_relaxed);

                        auto draw = TBUseRule50 ? 1 : 0;

                        auto value = wdl < -draw ? -VALUE_MATE + i32(MaxPlies + ss->ply) :
                                     wdl > +draw ? +VALUE_MATE - i32(MaxPlies + ss->ply) :
                                                    VALUE_ZERO + 2 * draw * wdl;

                        tte->save (key,
                                   MOVE_NONE,
                                   value_to_tt (value, ss->ply),
                                   VALUE_NONE,
                                   std::min<i16> (depth + 6, MaxPlies - 1),
                                   BOUND_EXACT);

                        return value;
                    }
                }
            }

            StateInfo si;

            // Step 5. Evaluate the position statically.
            if (in_check)
            {
                ss->static_eval = VALUE_NONE;
            }
            // Skipped when in check
            else
            {
                Value tt_eval;
                if (tt_hit)
                {
                    // Never assume anything on values stored in TT.
                    ss->static_eval = tt_eval =
                        VALUE_NONE != tte->eval () ?
                            tte->eval () :
                            evaluate (pos);
                    // Can tt_value be used as a better position evaluation?
                    if (   VALUE_NONE != tt_value
                        && BOUND_NONE != (tte->bound () & (tt_value > tt_eval ?
                                                            BOUND_LOWER :
                                                            BOUND_UPPER)))
                    {
                        tt_eval = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = tt_eval =
                        MOVE_NULL != last_move ?
                            evaluate (pos) :
                            -(ss-1)->static_eval + Tempo*2;

                    tte->save (key,
                               MOVE_NONE,
                               VALUE_NONE,
                               ss->static_eval,
                               DepthNone,
                               BOUND_NONE);
                }

                if (prun_node)
                {
                    assert(MOVE_NONE == ss->excluded_move);

                    // Step 6. Razoring sort of forward pruning where rather than
                    // skipping an entire subtree, search it to a reduced depth.
                    if (   !PVNode
                        && MaxRazorDepth > depth
                        //&& 0 == Limits.mate
                        && tt_eval + RazorMargins[depth] <= alfa)
                    {
                        if (1 >= depth)
                        {
                            return quien_search<false> (pos, ss, alfa, alfa+1);
                        }
                        auto alfa_margin = alfa - RazorMargins[depth];
                        assert(alfa_margin >= -VALUE_INFINITE);
                        auto value = quien_search<false> (pos, ss, alfa_margin, alfa_margin+1);
                        if (value <= alfa_margin)
                        {
                            return value;
                        }
                    }

                    // Step 7. Futility pruning: child node
                    // Betting that the opponent doesn't have a move that will reduce
                    // the score by more than futility margins [depth] if do a null move.
                    if (   !root_node
                        && 7 > depth
                        && tt_eval - 150*depth >= beta
                        && tt_eval < +VALUE_KNOWN_WIN // Don't return unproven wins.
                        //&& 0 == Limits.mate
                        && VALUE_ZERO != pos.si->non_pawn_material (pos.active))
                    {
                        return tt_eval;
                    }

                    // Step 8. Null move search with verification search.
                    if (   !PVNode
                        && tt_eval >= beta
                        //&& 0 == Limits.mate
                        && (   12 < depth
                            || ss->static_eval >= beta - 35*(depth - 6))
                        && VALUE_ZERO != pos.si->non_pawn_material (pos.active))
                    {
                        // Speculative prefetch as early as possible.
                        prefetch (TT.cluster_entry (  key
                                                    ^ RandZob.color_key
                                                    ^ (SQ_NO != pos.si->en_passant_sq ?
                                                        RandZob.en_passant_keys[_file (pos.si->en_passant_sq)] :
                                                        0)));

                        ss->played_move = MOVE_NULL;
                        ss->piece_destiny = &pos.thread->continuation[NO_PIECE][0];

                        pos.do_null_move (si);

                        // Null move dynamic reduction based on depth and static evaluation.
                        auto reduced_depth = i16(depth - (67*depth + 823) / 256 + std::min (i16(tt_eval - beta)/VALUE_MG_PAWN, 3));

                        auto null_value =
                            reduced_depth <= 0 ?
                                -quien_search<false> (pos, ss+1, -beta, -beta+1) :
                                -depth_search<false> (pos, ss+1, -beta, -beta+1, reduced_depth, !cut_node, false);
                        pos.undo_null_move ();

                        if (null_value >= beta)
                        {
                            bool unproven = null_value >= +VALUE_MATE_MAX_PLY;

                            // Don't do verification search at low depths.
                            if (   12 > depth
                                && abs (beta) < +VALUE_KNOWN_WIN)
                            {
                                // Don't return unproven wins.
                                return unproven ?
                                        beta : null_value;
                            }

                            // Do verification search at high depths.
                            auto value =
                                reduced_depth <= 0 ?
                                    quien_search<false> (pos, ss, beta-1, beta) :
                                    depth_search<false> (pos, ss, beta-1, beta, reduced_depth, false, false);

                            if (value >= beta)
                            {
                                // Don't return unproven wins.
                                return unproven ?
                                        beta : null_value;
                            }
                        }
                    }

                    // Step 9. ProbCut
                    // If good enough capture and a reduced search returns a value much above beta,
                    // then can (almost) safely prune the previous move.
                    if (   !PVNode
                        && 4 < depth
                        //&& 0 == Limits.mate
                        && abs (beta) < +VALUE_MATE_MAX_PLY)
                    {
                        auto beta_margin = std::min (beta + 200, +VALUE_INFINITE);
                        
                        assert(_ok (last_move));

                        // Initialize move picker (3) for the current position.
                        MovePicker move_picker (pos, tt_move, beta_margin - ss->static_eval);
                        // Loop through all legal moves until no moves remain or a beta cutoff occurs.
                        while (MOVE_NONE != (move = move_picker.next_move ()))
                        {
                            assert(pos.pseudo_legal (move)
                                && pos.legal (move));
                            assert(pos.capture_or_promotion (move));

                            // Speculative prefetch as early as possible.
                            prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                            ss->played_move = move;
                            ss->piece_destiny = &pos.thread->continuation[pos[org_sq (move)]][dst_sq (move)];

                            pos.do_move (move, si);

                            auto value = -depth_search<false> (pos, ss+1, -beta_margin, -beta_margin+1, depth - 4, !cut_node, true);

                            pos.undo_move (move);

                            if (value >= beta_margin)
                            {
                                return value;
                            }
                        }
                    }

                    // Step 10. Internal iterative deepening (IID).
                    if (   MOVE_NONE == tt_move
                        && 4 < depth
                        && (   PVNode
                            || ss->static_eval + 256 >= beta))
                    {
                        depth_search<PVNode> (pos, ss, alfa, beta, 3*depth/4 - 2, cut_node, false);

                        tte = TT.probe (key, tt_hit);
                        tt_move =
                            tt_hit
                         && MOVE_NONE != (move = tte->move ())
                         && pos.pseudo_legal (move)
                         && pos.legal (move) ?
                                move :
                                MOVE_NONE;
                        tt_value =
                            tt_hit ?
                                value_of_tt (tte->value (), ss->ply) :
                                VALUE_NONE;
                    }
                }
            }

            auto best_value = -VALUE_INFINITE;
            auto value = -VALUE_INFINITE;

            auto best_move = MOVE_NONE;

            bool singular_ext_node =
                   !root_node
                && tt_hit
                && MOVE_NONE != tt_move
                && VALUE_NONE != tt_value
                && MOVE_NONE == ss->excluded_move // Recursive singular search is not allowed.
                && 7 < depth
                && tte->depth () + 4 > depth
                && BOUND_NONE != (tte->bound () & BOUND_LOWER);

            bool improving =
                   (ss-2)->static_eval <= (ss-0)->static_eval
                || (ss-2)->static_eval == VALUE_NONE;

            u08 move_count = 0;

            Moves quiet_moves;
            quiet_moves.reserve (16);

            bool ttm_capture = false;

            const PieceDestinyHistory* piece_destiny[] = { (ss-1)->piece_destiny, (ss-2)->piece_destiny, (ss-3)->piece_destiny, (ss-4)->piece_destiny };
            // Initialize move picker (1) for the current position.
            MovePicker move_picker (pos, tt_move, depth, &pos.thread->butterfly, piece_destiny, ss->killer_moves, _ok (last_move) ? pos.thread->counter_moves[pos[fix_dst_sq (last_move)]][dst_sq (last_move)] : MOVE_NONE);
            // Step 11. Loop through moves
            // Loop through all legal moves until no moves remain or a beta cutoff occurs.
            while (MOVE_NONE != (move = move_picker.next_move ()))
            {
                assert(pos.pseudo_legal (move)
                    && pos.legal (move));

                if (   // Skip exclusion move
                       (move == ss->excluded_move)
                       // At root obey following rules:
                       // In "searchmoves" mode, skip moves not listed in RootMoves, as a consequence any illegal move is also skipped.
                       // In MultiPV mode, skip PV moves which have been already searched.
                    || (   root_node
                        && std::find (pos.thread->root_moves.begin () + pos.thread->pv_index,
                                      pos.thread->root_moves.end (), move) ==
                                      pos.thread->root_moves.end ()))
                {
                    continue;
                }

                ss->move_count = ++move_count;

                bool gives_check = pos.gives_check (move);
                bool capture_or_promotion = pos.capture_or_promotion (move);
                bool move_count_pruning =
                    MaxFutilityDepth > depth
                 && FutilityMoveCounts[improving][depth] <= move_count;

                auto mpc = pos[org_sq (move)];
                assert(NO_PIECE != mpc);
                auto dst = dst_sq (move);

                if (   root_node
                    && Threadpool.main_thread () == pos.thread)
                {
                    auto elapsed_time = Threadpool.main_thread ()->time_mgr.elapsed_time ();
                    if (elapsed_time > 3000)
                    {
                        auto &root_move = *std::find (pos.thread->root_moves.begin (), pos.thread->root_moves.end (), move);
                        sync_cout
                            << "info"
                            << " currmove " << move_to_can (move)
                            << " currmovenumber " << pos.thread->pv_index + move_count
                            << " maxmoves " << pos.thread->root_moves.size ()
                            << " depth " << depth
                            << " seldepth " << root_move.sel_depth
                            << " time " << elapsed_time
                            << sync_endl;
                    }
                }

                if (PVNode)
                {
                    (ss+1)->pv.clear ();
                }

                // Calculate new depth for this move
                i16 new_depth = depth - 1;

                // Step 12. Extensions

                // Singular extension (SE)
                // We extend the TT move if its value is much better than its siblings.
                // If all moves but one fail low on a search of (alfa-s, beta-s),
                // and just one fails high on (alfa, beta), then that move is singular and should be extended.
                // To verify this do a reduced search on all the other moves but the tt_move,
                // if result is lower than tt_value minus a margin then extend tt_move.
                if (   singular_ext_node
                    && move == tt_move)
                {
                    auto beta_margin = std::max (tt_value - 2*depth, -VALUE_MATE);

                    ss->excluded_move = move;
                    value = depth_search<false> (pos, ss, beta_margin-1, beta_margin, depth/2, cut_node, false);
                    ss->excluded_move = MOVE_NONE;

                    if (value < beta_margin)
                    {
                        new_depth += 1;
                    }
                }
                else
                // Check extension (CE)
                if (   gives_check
                    && !move_count_pruning
                    && pos.see_ge (move))
                {
                    new_depth += 1;
                }

                // Step 13. Pruning at shallow depth.
                if (   !root_node
                    //&& 0 == Limits.mate
                    && best_value > -VALUE_MATE_MAX_PLY
                    && VALUE_ZERO != pos.si->non_pawn_material (pos.active))
                {
                    if (   !capture_or_promotion
                        && !gives_check
                            // Advance pawn push.
                        && !(   PAWN == ptype (mpc)
                             && rel_rank (pos.active, org_sq (move)) > R_4
                             && pos.si->non_pawn_material () < Value(5000)))
                    {
                        // Move count based pruning.
                        if (move_count_pruning)
                        {
                            move_picker.skip_quiets = true;
                            continue;
                        }

                        // Reduced depth of the next LMR search.
                        i16 lmr_depth = i16(std::max (new_depth - reduction_depth (PVNode, improving, depth, move_count), 0));
                        if (    // Countermoves based pruning.
                               (   3 > lmr_depth
                                && (*piece_destiny[0])[mpc][dst] < CounterMovePruneThreshold
                                && (*piece_destiny[1])[mpc][dst] < CounterMovePruneThreshold)
                                // Futility pruning: parent node.
                            || (   7 > lmr_depth
                                && !in_check
                                && ss->static_eval + 200*lmr_depth + 256 <= alfa)
                                // SEE based pruning: -ve SEE
                            || (   8 > lmr_depth
                                && !pos.see_ge (move, Value(-35*lmr_depth*lmr_depth))))
                        {
                            continue;
                        }
                    }
                    else
                    // SEE based pruning.
                    if (   7 > depth
                        && new_depth < depth
                        && !pos.see_ge (move, -VALUE_EG_PAWN*i32(depth)))
                    {
                        continue;
                    }
                }

                // Speculative prefetch as early as possible.
                prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                // Update the current move (this must be done after singular extension search).
                ss->played_move = move;
                ss->piece_destiny = &pos.thread->continuation[mpc][dst];

                // Step 14. Make the move.
                pos.do_move (move, si, gives_check);

                if (   capture_or_promotion
                    && move == tt_move)
                {
                    ttm_capture = true;
                }

                bool full_depth_search;
                // Step 15. Reduced depth search (LMR).
                // If the move fails high will be re-searched at full depth.
                if (   2 < depth
                    && 1 < move_count
                    && (   move_count_pruning
                        || !capture_or_promotion))
                {
                    i16 reduce_depth = reduction_depth (PVNode, improving, depth, move_count);

                    if (capture_or_promotion)
                    {
                        reduce_depth -= 1;
                    }
                    else
                    {
                        assert(PROMOTE != mtype (move));

                        // Increase reduction if tt_move is a capture
                        if (ttm_capture)
                        {
                            reduce_depth += 1;
                        }

                        // Increase reduction for cut nodes
                        if (cut_node)
                        {
                            reduce_depth += 2;
                        }
                        else
                        // Decrease reduction for moves that escape a capture in no-cut nodes.
                        // Filter out castling moves, because they are coded as "king captures rook" and hence break mk_move().
                        if (   NORMAL == mtype (move)
                            && !pos.see_ge (mk_move<NORMAL> (dst, org_sq (move))))
                        {
                            reduce_depth -= 2;
                        }

                        ss->statistics =
                              pos.thread->butterfly[~pos.active][move_pp (move)]
                            + (*piece_destiny[0])[mpc][dst]
                            + (*piece_destiny[1])[mpc][dst]
                            + (*piece_destiny[3])[mpc][dst]
                            - 4000; // Correction factor

                        // Decrease/Increase reduction by comparing opponent's stat value
                        if (   (ss-0)->statistics > 0
                            && (ss-1)->statistics < 0)
                        {
                            reduce_depth -= 1;
                        }
                        else
                        if (   (ss-0)->statistics < 0
                            && (ss-1)->statistics > 0)
                        {
                            reduce_depth += 1;
                        }

                        // Decrease/Increase reduction for moves with +/-ve history value
                        reduce_depth -= i16((ss)->statistics / 20000);
                    }

                    if (reduce_depth < 0)
                    {
                        reduce_depth = 0;
                    }
                    else
                    if (reduce_depth > new_depth - 1)
                    {
                        reduce_depth = new_depth - 1;
                    }

                    value = -depth_search<false> (pos, ss+1, -alfa-1, -alfa, new_depth - reduce_depth, true, true);

                    full_depth_search =
                        alfa < value
                     && 0 != reduce_depth;
                }
                else
                {
                    full_depth_search = !PVNode
                                     || 1 < move_count;
                }

                // Step 16. Full depth search when LMR is skipped or fails high.
                if (full_depth_search)
                {
                    value =
                        new_depth <= 0 ?
                            -quien_search<false> (pos, ss+1, -alfa-1, -alfa) :
                            -depth_search<false> (pos, ss+1, -alfa-1, -alfa, new_depth, !cut_node, true);
                }

                // Do a full PV search on following conditions:
                // - 'first' move.
                // - 'fail high (alfa < value)' move (only if value < beta otherwise let the parent node fail low with alfa >= value and try another move).
                if (   PVNode
                    && (   1 == move_count
                        || (   alfa < value
                            && (   root_node
                                || value < beta))))
                {
                    (ss+1)->pv.clear ();

                    value =
                        new_depth <= 0 ?
                            -quien_search<true> (pos, ss+1, -beta, -alfa) :
                            -depth_search<true> (pos, ss+1, -beta, -alfa, new_depth, false, true);
                }

                // Step 17. Undo move.
                pos.undo_move (move);

                assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Step 18. Check for the new best move.
                // Finished searching the move. If a stop or a cutoff occurred,
                // the return value of the search cannot be trusted,
                // and return immediately without updating best move, PV and TT.
                if (Threadpool.stop.load (std::memory_order::memory_order_relaxed))
                {
                    return VALUE_ZERO;
                }

                if (root_node)
                {
                    assert(std::find (pos.thread->root_moves.begin (), pos.thread->root_moves.end (), move) != pos.thread->root_moves.end ());
                    auto &root_move = *std::find (pos.thread->root_moves.begin (), pos.thread->root_moves.end (), move);
                    // First PV move or new best move?
                    if (   1 == move_count
                        || alfa < value)
                    {
                        root_move.resize (1);
                        for (auto m : (ss+1)->pv)
                        {
                            root_move += m;
                        }
                        root_move.new_value = value;
                        root_move.sel_depth = pos.thread->sel_depth;

                        // Record how often the best move has been changed in each iteration.
                        // This information is used for time management:
                        // When the best move changes frequently, allocate some more time.
                        if (   1 < move_count
                            && Limits.use_time_management ()
                            && Threadpool.main_thread () == pos.thread)
                        {
                            ++Threadpool.main_thread ()->best_move_change;
                        }
                    }
                    else
                    {
                        // All other moves but the PV are set to the lowest value, this
                        // is not a problem when sorting becuase sort is stable and move
                        // position in the list is preserved, just the PV is pushed up.
                        root_move.new_value = -VALUE_INFINITE;
                    }
                }

                // Step 19. Check best value.
                if (best_value < value)
                {
                    best_value = value;

                    if (alfa < value)
                    {
                        best_move = move;

                        // Update pv even in fail-high case.
                        if (   PVNode
                            && !root_node)
                        {
                            update_pv (ss->pv, move, (ss+1)->pv);
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            break;
                        }
                        assert(value < beta);

                        // Update alfa! Always alfa < beta.
                        if (PVNode)
                        {
                            alfa = value;
                        }
                    }
                }

                if (   !capture_or_promotion
                    && move != best_move)
                {
                    quiet_moves.push_back (move);
                }
            }

            assert(!in_check
                || 0 != move_count
                || MOVE_NONE != ss->excluded_move
                || 0 == MoveList<GenType::LEGAL> (pos).size ());

            // Step 20. Check for checkmate and stalemate.
            // If all possible moves have been searched and if there are no legal moves,
            // If in a singular extension search then return a fail low score (alfa).
            // Otherwise it must be a checkmate or a stalemate, so return value accordingly.
            if (0 == move_count)
            {
                best_value =
                    MOVE_NONE != ss->excluded_move ?
                        alfa :
                        in_check ?
                            mated_in (ss->ply) :
                            DrawValue[pos.active];
            }
            else
            {
                // Quiet best move: update move sorting heuristics.
                if (MOVE_NONE != best_move)
                {
                    if (!pos.capture_or_promotion (best_move))
                    {
                        auto bonus = stat_bonus (depth);
                        update_histories (ss, pos, best_move, bonus);
                        // Decrease all the other played quiet moves.
                        for (auto qm : quiet_moves)
                        {
                            pos.thread->butterfly.update (pos.active, qm, -bonus);
                            update_continuation_histories (ss, pos[org_sq (qm)], dst_sq (qm), -bonus);
                        }
                    }
                    // Penalty for a quiet best move in previous ply when it gets refuted.
                    if (   1 == (ss-1)->move_count
                        && _ok (last_move)
                        && NONE == pos.si->capture
                        && !pos.si->promotion)
                    {
                        update_continuation_histories (ss-1, pos[fix_dst_sq (last_move)], dst_sq (last_move), -stat_bonus (depth + 1));
                    }
                }
                else
                // Bonus for prior countermove that caused the fail low.
                if (   2 < depth
                    && _ok (last_move)
                    && NONE == pos.si->capture
                    && !pos.si->promotion)
                {
                    update_continuation_histories (ss-1, pos[fix_dst_sq (last_move)], dst_sq (last_move), stat_bonus (depth));
                }
            }
            
            if (MOVE_NONE == ss->excluded_move)
            {
                tte->save (key,
                           best_move,
                           value_to_tt (best_value, ss->ply),
                           ss->static_eval,
                           depth,
                           best_value >= beta ?
                               BOUND_LOWER :
                                  PVNode
                               && MOVE_NONE != best_move ?
                                   BOUND_EXACT :
                                   BOUND_UPPER);
            }
            assert(-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }
    }

    // Utility to verify move generation.
    // All the leaf nodes up to the given depth are generated, and the sum is returned.
    template<bool RootNode>
    u64 perft (Position &pos, i16 depth)
    {
        u64 leaf_nodes = 0;
        i16 move_count = 0;
        for (const auto &vm : MoveList<GenType::LEGAL> (pos))
        {
            u64 cum_nodes;
            if (   RootNode
                && 1 >= depth)
            {
                cum_nodes = 1;
            }
            else
            {
                StateInfo si;
                pos.do_move (vm.move, si);
                cum_nodes =
                    2 < depth ?
                        perft<false> (pos, depth - 1) :
                        MoveList<GenType::LEGAL> (pos).size ();
                pos.undo_move (vm.move);
            }

            if (RootNode)
            {
                sync_cout
                    << std::right
                    << std::setfill ('0')
                    << std::setw (2)
                    << ++move_count
                    << " "
                    << std::left
                    << std::setfill (' ')
                    << std::setw (7)
                    << 
                       //move_to_can (vm.move)
                       move_to_san (vm.move, pos)
                    << std::right
                    << std::setfill ('.')
                    << std::setw (16)
                    << cum_nodes
                    << std::setfill (' ')
                    << std::left
                    << sync_endl;
            }

            leaf_nodes += cum_nodes;
        }
        return leaf_nodes;
    }
    // Explicit template instantiations
    template u64 perft<false> (Position&, i16);
    template u64 perft<true > (Position&, i16);

    // Initialize lookup tables during startup.
    void initialize ()
    {
        for (i16 d = 0; d < MaxFutilityDepth; ++d)
        {
            FutilityMoveCounts[0][d] = u08(0.74 * std::pow (d, 1.78) + 2.4);
            FutilityMoveCounts[1][d] = u08(1.00 * std::pow (d, 2.00) + 5.0);
        }
        for (i08 imp = 0; imp < 2; ++imp)
        {
            for (i16 d = 1; d < MaxReductionDepth; ++d)
            {
                for (i08 mc = 1; mc < MaxReductionMoveCount; ++mc)
                {
                    auto r = log (d) * log (mc) / 1.95;
                    ReductionDepths[0][imp][d][mc] = i16(std::round (r));
                    ReductionDepths[1][imp][d][mc] = i16(std::max (ReductionDepths[0][imp][d][mc] - 1, 0));
                    if (   0 == imp
                        && ReductionDepths[0][imp][d][mc] >= 2)
                    {
                        ReductionDepths[0][imp][d][mc] += 1;
                    }
                }
            }
        }
    }
    // Resets search state to its initial value, to obtain reproducible results.
    void clear ()
    {
        if (!RetainHash)
        {
            TT.clear ();
            Threadpool.clear ();
        }
    }
}

namespace Threading {

    using namespace Searcher;

    const u08 SkipIndex = 20;
    const u08 SkipSize [SkipIndex] = { 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4 };
    const u08 SkipPhase[SkipIndex] = { 0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7 };

    // Thread iterative deepening loop function.
    // It calls depth_search() repeatedly with increasing depth until
    // - the force stop requested.
    // - the allocated thinking time has been consumed.
    // - the maximum search depth is reached.
    void Thread::search ()
    {
        Stack stacks[MaxPlies + 7]; // To allow referencing (ss-4) and (ss+2)
        for (auto ss = stacks; ss < stacks + MaxPlies + 7; ++ss)
        {
            ss->ply = i16(ss - stacks - 3);
            ss->played_move = MOVE_NONE;
            ss->excluded_move = MOVE_NONE;
            std::fill_n (ss->killer_moves, MaxKillers, MOVE_NONE);
            ss->static_eval = VALUE_ZERO;
            ss->statistics = 0;
            ss->move_count = 0;
            ss->piece_destiny = &continuation[NO_PIECE][0];
        }

        auto *main_thread =
            this == Threadpool.main_thread () ?
                Threadpool.main_thread () :
                nullptr;

        auto best_value = VALUE_ZERO
           , window = VALUE_ZERO
           , alfa = -VALUE_INFINITE
           , beta = +VALUE_INFINITE;

        // Iterative deepening loop until requested to stop or the target depth is reached.
        while (   ++running_depth < MaxPlies
               && !Threadpool.stop
               && (   nullptr == main_thread
                   || 0 == Limits.depth
                   || running_depth <= Limits.depth))
        {
            if (nullptr != main_thread)
            {
                if (Limits.use_time_management ())
                {
                    main_thread->failed_low = false;
                    // Age out PV variability metric
                    main_thread->best_move_change *= 0.505;
                }
            }
            else
            {
                // Distribute search depths across the threads.
                assert(0 != index);
                int i = (index - 1) % SkipIndex;
                if (0 != ((running_depth + root_pos.ply + SkipPhase[i]) / SkipSize[i]) % 2)
                {
                    continue;
                }
            }

            // Save the last iteration's values before first PV line is searched.
            for (auto &rm : root_moves)
            {
                rm.old_value = rm.new_value;
            }

            // MultiPV loop. Perform a full root search for each PV line.
            for (   pv_index = 0;
                    !Threadpool.stop
                 && pv_index < Threadpool.pv_limit;
                    ++pv_index)
            {
                // Reset UCI info sel_depth for each depth and each PV line
                sel_depth = 0;

                // Reset aspiration window starting size.
                if (running_depth > 4)
                {
                    window = Value(18);
                    alfa = std::max (root_moves[pv_index].old_value - window, -VALUE_INFINITE);
                    beta = std::min (root_moves[pv_index].old_value + window, +VALUE_INFINITE);
                }

                // Start with a small aspiration window and, in case of fail high/low,
                // research with bigger window until not failing high/low anymore.
                while (true)
                {
                    best_value = depth_search<true> (root_pos, stacks+4, alfa, beta, running_depth, false, true);

                    // Bring the best move to the front. It is critical that sorting is
                    // done with a stable algorithm because all the values but the first
                    // and eventually the new best one are set to -VALUE_INFINITE and
                    // want to keep the same order for all the moves but the new PV
                    // that goes to the front. Note that in case of MultiPV search
                    // the already searched PV lines are preserved.
                    std::stable_sort (root_moves.begin () + pv_index, root_moves.end ());

                    // If search has been stopped, break immediately.
                    // Sorting and writing PV back to TT is safe becuase
                    // root moves is still valid, although refers to the previous iteration.
                    if (Threadpool.stop)
                    {
                        break;
                    }

                    if (nullptr != main_thread)
                    {
                        // Give some update before to re-search.
                        if (   1 == Threadpool.pv_limit
                            && (best_value <= alfa || beta <= best_value)
                            && main_thread->time_mgr.elapsed_time () > 3000)
                        {
                            sync_cout << multipv_info (this, running_depth, alfa, beta) << sync_endl;
                        }
                    }
                    // If fail low set new bounds.
                    if (best_value <= alfa)
                    {
                        beta = (alfa + beta) / 2;
                        alfa = std::max (best_value - window, -VALUE_INFINITE);

                        if (nullptr != main_thread)
                        {
                            if (Limits.use_time_management ())
                            {
                                main_thread->failed_low = true;
                            }
                            Threadpool.stop_on_ponderhit = false;
                        }
                    }
                    else
                    // If fail high set new bounds.
                    if (beta <= best_value)
                    {
                        // NOTE:: Don't change alfa = (alfa + beta) / 2;
                        beta = std::min (best_value + window, +VALUE_INFINITE);
                    }
                    // Otherwise exit the loop
                    else
                    {
                        break;
                    }

                    window += window / 4 + 5;

                    assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
                }

                // Sort the PV lines searched so far and update the GUI.
                std::stable_sort (root_moves.begin (), root_moves.begin () + pv_index + 1);

                if (nullptr != main_thread)
                {
                    if (   Threadpool.stop
                        || Threadpool.pv_limit == pv_index + 1
                        || main_thread->time_mgr.elapsed_time () > 3000)
                    {
                        sync_cout << multipv_info (this, running_depth, alfa, beta) << sync_endl;
                    }
                }
            }

            if (!Threadpool.stop)
            {
                finished_depth = running_depth;
            }

            //if (0 != ContemptValue)
            //{
            //    auto valued_contempt = Value(i32(root_moves[0].new_value)/ContemptValue);
            //    DrawValue[ root_pos.active] = BaseContempt[ root_pos.active] - valued_contempt;
            //    DrawValue[~root_pos.active] = BaseContempt[~root_pos.active] + valued_contempt;
            //}

            if (nullptr != main_thread)
            {
                // If skill level is enabled and can pick move, pick a sub-optimal best move.
                if (   main_thread->skill_mgr.enabled ()
                    && main_thread->skill_mgr.can_pick (running_depth))
                {
                    main_thread->skill_mgr.clear ();
                    main_thread->skill_mgr.pick_best_move (main_thread->root_moves);
                }

                if (OutputStream.is_open ())
                {
                    OutputStream << pretty_pv_info (this) << std::endl;
                }

                if (   !Threadpool.stop
                    && !Threadpool.stop_on_ponderhit)
                {
                    // Stop the search early:
                    bool stop = false;

                    // Have time for the next iteration? Can stop searching now?
                    if (Limits.use_time_management ())
                    {
                        auto &root_move = root_moves[0];
                        // Stop the search
                        // -If there is only one legal move available
                        // -If all of the available time has been used
                        // -If matched an easy move from the previous search and just did a fast verification.
                        if (   1 == root_moves.size ()
                            || (  main_thread->time_mgr.elapsed_time () >
                                  main_thread->time_mgr.optimum_time
                                        // Unstable factor
                                        * (1.0 + main_thread->best_move_change)
                                        // Improving factor
                                        * std::min (1.1385,
                                          std::max (0.3646,
                                                    0.5685
                                                  + 0.1895 * (main_thread->failed_low ? 1 : 0)
                                                  - 0.0096 * (VALUE_NONE != main_thread->last_value ? best_value - main_thread->last_value : 0))))
                            || (main_thread->easy_played =
                                    (   root_move == main_thread->easy_move
                                     && main_thread->best_move_change < 0.030
                                     && main_thread->time_mgr.elapsed_time () >
                                        main_thread->time_mgr.optimum_time * 0.1136), main_thread->easy_played))
                        {
                            stop = true;
                        }

                        if (3 <= root_move.size ())
                        {
                            main_thread->move_mgr.update (root_pos, root_move);
                        }
                        else
                        {
                            main_thread->move_mgr.clear ();
                        }
                    }
                    else
                    // Have found a "mate in <x>"?
                    if (   0 != Limits.mate
                        && best_value >= +VALUE_MATE - 2*Limits.mate)
                    {
                        stop = true;
                    }

                    if (stop)
                    {
                        // If allowed to ponder do not stop the search now but
                        // keep pondering until GUI sends "ponderhit" or "stop".
                        if (Threadpool.ponder)
                        {
                            Threadpool.stop_on_ponderhit = true;
                        }
                        else
                        {
                            Threadpool.stop = true;
                        }
                    }
                }
            }
        }
    }

    // Main thread search function.
    // It searches from root position and outputs the "bestmove"/"ponder".
    void MainThread::search ()
    {
        static Book book; // Defined static to initialize the PRNG only once
        assert(Threadpool.main_thread () == this
            && index == 0);

        check_count = 0;

        if (!white_spaces (OutputFile))
        {
            OutputStream.open (OutputFile, ios_base::out|ios_base::app);
            if (OutputStream.is_open ())
            {
                OutputStream
                    << std::boolalpha
                    << "RootPos  : " << root_pos.fen () << "\n"
                    << "MaxMoves : " << root_moves.size () << "\n"
                    << "ClockTime: " << Limits.clock[root_pos.active].time << " ms\n"
                    << "ClockInc : " << Limits.clock[root_pos.active].inc << " ms\n"
                    << "MovesToGo: " << Limits.movestogo+0 << "\n"
                    << "MoveTime : " << Limits.movetime << " ms\n"
                    << "Depth    : " << Limits.depth << "\n"
                    << "Infinite : " << Limits.infinite << "\n"
                    << "Ponder   : " << Threadpool.ponder << "\n"
                    << " Depth Score    Time       Nodes PV\n"
                    << "-----------------------------------------------------------"
                    << std::noboolalpha
                    << std::endl;
            }
        }

        if (Limits.use_time_management ())
        {
            // When playing in 'Nodes as Time' mode, then convert from time to nodes, and use values in time management.
            // WARNING: Given NodesTime (nodes per millisecond) must be much lower then the real engine speed to avoid time losses.
            if (0 != NodesTime)
            {
                // Only once at after ucinewgame
                if (0 == time_mgr.available_nodes)
                {
                    time_mgr.available_nodes = Limits.clock[root_pos.active].time * NodesTime;
                }
                // Convert from millisecs to nodes
                Limits.clock[root_pos.active].time = time_mgr.available_nodes;
                Limits.clock[root_pos.active].inc *= NodesTime;
            }

            // Initialize the time manager before searching.
            time_mgr.initialize (root_pos.active, root_pos.ply);
        }

        Transposition::Entry::Generation = u08((root_pos.ply + 1) << 2);
        assert(0 == (Transposition::Entry::Generation & 0x03));

        bool voting = false;

        if (root_moves.empty ())
        {
            root_moves += RootMove ();

            sync_cout
                << "info"
                << " depth " << 0
                << " score " << to_string (0 != root_pos.si->checkers ? -VALUE_MATE : VALUE_DRAW)
                << " time " << 0
                << sync_endl;
        }
        else
        {
            // Check if can play with own book.
            if (   OwnBook
                && !white_spaces (BookFile)
                && (   0 == BookUptoMove
                    || root_pos.move_num () <= BookUptoMove)
                && 0 == Limits.mate
                && !Limits.infinite)
            {
                book.open (BookFile, ios_base::in);
                bool found = false;
                auto book_best_move = book.probe_move (root_pos, BookMoveBest);
                if (MOVE_NONE != book_best_move)
                {
                    auto itr = std::find (root_moves.begin (), root_moves.end (), book_best_move);
                    if (itr != root_moves.end ())
                    {
                        auto &root_move = root_moves[0];
                        std::swap (root_move, *itr);
                        StateInfo si;
                        root_pos.do_move (book_best_move, si);
                        auto book_ponder_move = book.probe_move (root_pos, BookMoveBest);
                        root_move += book_ponder_move;
                        root_pos.undo_move (book_best_move);
                        found = true;
                    }
                }
                book.close ();
                if (found)
                {
                    goto finish;
                }
            }

            voting = true;

            i16 timed_contempt = 0;
            i64 diff_time;
            if (   Limits.use_time_management ()
                && 0 != ContemptTime
                && 0 != (diff_time = i64(  Limits.clock[ root_pos.active].time
                                         - Limits.clock[~root_pos.active].time)/1000))
            {
                timed_contempt = i16(diff_time/ContemptTime);
            }

            auto contempt = cp_to_value ((FixedContempt + timed_contempt) / 100.0);
            DrawValue[ root_pos.active] = BaseContempt[ root_pos.active] = VALUE_DRAW - contempt;
            DrawValue[~root_pos.active] = BaseContempt[~root_pos.active] = VALUE_DRAW + contempt;

            if (Limits.use_time_management ())
            {
                easy_move = move_mgr.easy_move (root_pos.si->posi_key);
                move_mgr.clear ();
                easy_played = false;
                failed_low = false;
                best_move_change = 0.0;
            }
            if (skill_mgr.enabled ())
            {
                skill_mgr.clear ();
            }

            // Have to play with skill handicap?
            // In this case enable MultiPV search by skill pv size
            // that will use behind the scenes to get a set of possible moves.
            Threadpool.pv_limit = std::min (std::max (MultiPV, u08(skill_mgr.enabled () ? 4 : 1)), u08(root_moves.size ()));

            for (auto *th : Threadpool)
            {
                if (th != this)
                {
                    th->start_searching (false);
                }
            }

            Thread::search (); // Let's start searching !

            // Clear any candidate easy move
            if (   Limits.use_time_management ()
                && (// Prevents consecutive fast moves
                       easy_played
                    // Unstable for the last search iterations
                    || move_mgr.stable_count < 6))
            {
                move_mgr.clear ();
            }
            // Swap best PV line with the sub-optimal one if skill level is enabled
            if (skill_mgr.enabled ())
            {
                skill_mgr.pick_best_move (root_moves);
                if (MOVE_NONE != skill_mgr.best_move)
                {
                    auto itr = std::find (root_moves.begin (), root_moves.end (), skill_mgr.best_move);
                    if (itr != root_moves.end ())
                    {
                        std::swap (root_moves[0], *itr);
                    }
                }
            }
        }

    finish:

        if (Limits.use_time_management ())
        {
            // When playing in 'Nodes as Time' mode, update the time manager after searching.
            if (0 != NodesTime)
            {
                time_mgr.available_nodes += Limits.clock[root_pos.active].inc - Threadpool.nodes ();
            }
        }

        // When reach max depth arrive here even without Force Stop is raised,
        // but if are pondering or in infinite search, according to UCI protocol,
        // shouldn't print the best move before the GUI sends a "stop" or "ponderhit" command.
        // Simply wait here until GUI sends one of those commands (that raise Force Stop).
        if (   !Threadpool.stop
            && (   Limits.infinite
                || Threadpool.ponder))
        {
            Threadpool.stop_on_ponderhit = true;
            wait_until (Threadpool.stop);
        }

        Thread *best_thread = this;
        if (voting)
        {
            // Stop the threads if not already stopped.
            Threadpool.stop = true;
            // Wait until all threads have finished.
            for (auto *th : Threadpool)
            {
                if (th != this)
                {
                    th->wait_while (th->searching);
                }
            }
            // Check if there are deeper thread than main thread.
            if (   1 == Threadpool.pv_limit
                && !easy_played
                && 0 == Limits.depth // Depth limit search don't use deeper thread
                && MOVE_NONE != root_moves[0][0]
                && !skill_mgr.enabled ())
            {
                best_thread = Threadpool.best_thread ();
                // If best thread is not main thread send new PV.
                if (best_thread != this)
                {
                    sync_cout << multipv_info (best_thread, best_thread->finished_depth, -VALUE_INFINITE, +VALUE_INFINITE) << sync_endl;
                }
            }
        }

        assert(!best_thread->root_moves.empty ()
            && !best_thread->root_moves[0].empty ());

        auto &root_move = best_thread->root_moves[0];

        if (Limits.use_time_management ())
        {
            last_value = root_move.new_value;
        }

        auto best_move = root_move[0];
        auto ponder_move =
            MOVE_NONE != best_move
         && (   root_move.size () > 1
             || root_move.extract_ponder_move_from_tt (root_pos)) ?
                root_move[1] :
                MOVE_NONE;
        assert(MOVE_NONE != best_move
            || (MOVE_NONE == best_move
             && MOVE_NONE == ponder_move));

        if (OutputStream.is_open ())
        {
            auto total_nodes = Threadpool.nodes ();
            auto elapsed_time = std::max (time_mgr.elapsed_time (), 1ULL);
            OutputStream
                << "Nodes      : " << total_nodes << " N\n"
                << "Time       : " << elapsed_time << " ms\n"
                << "Speed      : " << total_nodes * 1000 / elapsed_time << " N/s\n"
                << "Hash-full  : " << TT.hash_full () << "\n"
                << "Best Move  : " << move_to_san (best_move, root_pos) << "\n"
                << "Ponder Move: ";
            if (MOVE_NONE != best_move)
            {
                StateInfo si;
                root_pos.do_move (best_move, si);
                OutputStream << move_to_san (ponder_move, root_pos);
                root_pos.undo_move (best_move);
            }
            else
            {
                OutputStream << "(none)";
            }
            OutputStream << "\n" << std::endl;
            OutputStream.close ();
        }

        // Best move could be MOVE_NONE when searching on a stalemate position.
        sync_cout
            << "bestmove " << move_to_can (best_move)
            << " ponder " << move_to_can (ponder_move)
            << sync_endl;
    }
    // Used to detect when out of available limits and thus stop the search, also print debug info.
    void MainThread::check_limits ()
    {
        static TimePoint last_time = now ();

        if (--check_count >= 0)
        {
            return;
        }
        // At low node count increase the checking rate otherwise use a default value.
        check_count = i16(0 != Limits.nodes ? std::min (std::max (i32(std::round ((double) Limits.nodes / 0x1000)), 1), 0x1000) : 0x1000);
        assert(0 != check_count);

        auto elapsed_time = time_mgr.elapsed_time ();
        TimePoint tick = Limits.start_time + elapsed_time;

        if (last_time <= tick - 1000)
        {
            last_time = tick;
            
            dbg_print ();
        }

        // Do not stop until told so by the GUI.
        if (   Limits.infinite
            || Threadpool.ponder)
        {
            return;
        }

        if (   (   Limits.use_time_management ()
                && elapsed_time > time_mgr.maximum_time - 10) // 2 * Timer Resolution
            || (   0 != Limits.movetime
                && elapsed_time >= Limits.movetime)
            || (   0 != Limits.nodes
                && Threadpool.nodes () >= Limits.nodes))
        {
            Threadpool.stop = true;
        }
    }
}
