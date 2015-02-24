#ifndef _MOVE_PICKER_H_INC_
#define _MOVE_PICKER_H_INC_

#include "Type.h"
#include "Position.h"
#include "MoveGenerator.h"
#include "Searcher.h"

namespace MovePick {

    using namespace MoveGen;

    // The Stats struct stores moves statistics.

    // Gain records the move's best evaluation gain from one ply to the next and is used
    // for pruning decisions.
    // Entries are stored according only to moving piece and destination square,
    // in particular two moves with different origin but same destination and same piece will be considered identical.
    struct GainStats
    {

    private:
        Value _values[PIECE_NO][SQ_NO];

    public:

        inline void clear ()
        {
            std::fill (*_values, *_values + sizeof (_values)/sizeof (**_values), VALUE_ZERO);
        }

        inline void update (const Position &pos, Move m, Value g)
        {
            Square s = dst_sq (m);
            Piece  p = pos[s];
            _values[p][s] = std::max (g, _values[p][s] - 1);
        }

        inline const Value* operator[] (Piece p) const { return _values[p]; }
    };

    // History records how often different moves have been successful or unsuccessful during the
    // current search and is used for reduction and move ordering decisions.
    // Entries are stored according only to moving piece and destination square,
    // in particular two moves with different origin but same destination and same piece will be considered identical.
    struct HistoryStats
    {

    private:
        Value _values[PIECE_NO][SQ_NO];

    public:

        static const Value MaxValue = Value(+0x100);

        inline void clear ()
        {
            std::fill (*_values, *_values + sizeof (_values)/sizeof (**_values), VALUE_ZERO);
        }

        inline void update (const Position &pos, Move m, Value v)
        {
            Square s = dst_sq (m);
            Piece  p = pos[org_sq (m)];
            if (abs (_values[p][s] + v) < MaxValue) _values[p][s] += v;
        }

        inline const Value* operator[] (Piece p) const { return _values[p]; }
    };

    // CounterMoveStats & FollowupMoveStats store the move that refute a previous one.
    // Entries are stored according only to moving piece and destination square,
    // in particular two moves with different origin but same destination and same piece will be considered identical.
    struct MoveStats
    {

    private:
        Move _moves[PIECE_NO][SQ_NO][2];

    public:

        inline void clear ()
        {
            std::fill (**_moves, **_moves + sizeof (_moves)/sizeof (***_moves), MOVE_NONE);
        }

        inline void update (const Position &pos, Move m1, Move m2)
        {
            Square s = dst_sq (m1);
            Piece  p = pos[s];
            if (_moves[p][s][0] != m2)
            {
                _moves[p][s][1] = _moves[p][s][0];
                _moves[p][s][0] = m2;
            }
        }

        inline Move* moves (const Position &pos, Square s)
        {
            return _moves[pos[s]][s];
        }
    };


    // MovePicker class is used to pick one pseudo legal move at a time from the
    // current position. The most important method is next_move(), which returns a
    // new pseudo legal move each time it is called, until there are no moves left,
    // when MOVE_NONE is returned. In order to improve the efficiency of the alpha
    // beta algorithm, MovePicker attempts to return the moves which are most likely
    // to get a cut-off first.
    class MovePicker
    {

    private:

        ValMove  moves[MAX_MOVES]
            ,   *cur
            ,   *end
            ,   *quiets_end
            ,   *bad_captures_end;

        const Position &pos;
        HistoryStats &history;

        Searcher::Stack *ss;

        Move   killers[6]
            ,  *counter_moves
            ,  *followup_moves
            ,  *kcur
            ,  *kend;
        Bitboard killers_org
            ,    killers_dst;
        u08      killers_size;

        Move    tt_move;
        Depth   depth;

        Square  recapture_sq;

        Value   capture_threshold;

        u08     stage;

        MovePicker& operator= (const MovePicker &); // Silence a warning under MSVC

        template<GenT GT>
        // value() assign a numerical move ordering score to each move in a move list.
        // The moves with highest scores will be picked first.
        void value ();

        void generate_next_stage ();

    public:

        MovePicker (const Position&, HistoryStats&, Move, Depth, Move*, Move*, Searcher::Stack*);
        MovePicker (const Position&, HistoryStats&, Move, Depth, Square);
        MovePicker (const Position&, HistoryStats&, Move, PieceT);

        // Picks and moves to the front the best move in the range [cur, end],
        // it is faster than sorting all the moves in advance when moves are few, as
        // normally are the possible captures.
        inline ValMove* pick_best ()
        {
            std::swap (*cur, *std::max_element (cur, end));
            return cur++;
        }

        template<bool SPNode>
        Move next_move ();

    };

}

#endif // _MOVE_PICKER_H_INC_
