#include "Notation.h"

#include <sstream>
#include <iomanip>

#include "Position.h"
#include "MoveGenerator.h"
#include "Time.h"

namespace Notation {

    using namespace std;
    using namespace BitBoard;
    using namespace MoveGenerator;
    using namespace Time;

    namespace {

        // value to string
        const string pretty_value (Value v)
        {
            ostringstream os;

            if (abs (v) < VALUE_MATES_IN_MAX_PLY)
            {
                os << setprecision (2) << fixed << showpos << double (v) / VALUE_MG_PAWN;
            }
            else
            {
                if (v > VALUE_ZERO) //if (v >= VALUE_MATES_IN_MAX_PLY)
                {
                    os <<  "#" << int32_t (VALUE_MATE - v + 1) / 2;
                }
                else                //if (v <= VALUE_MATED_IN_MAX_PLY)
                {
                    os << "-#" << int32_t (VALUE_MATE + v + 0) / 2;
                }
            }

            return os.str ();
        }

        // time to string
        const string pretty_time (uint64_t msecs)
        {
            const uint32_t MSecMinute = M_SEC * 60;
            const uint32_t MSecHour   = MSecMinute * 60;

            uint64_t hours   =   msecs / MSecHour;
            uint64_t minutes =  (msecs % MSecHour) / MSecMinute;
            uint64_t seconds = ((msecs % MSecHour) % MSecMinute) / M_SEC;

            ostringstream os;

            if (hours) os << hours << ':';
            os  << setfill ('0')
                << setw (2) << minutes << ':'
                << setw (2) << seconds;

            return os.str ();
        }

    }

    // Ambiguity if more then one piece of same type can reach 'dst' with a legal move.
    // NOTE: for pawns it is not needed because 'org' file is explicit.
    AmbiguityT ambiguity (Move m, const Position &pos)
    {
        ASSERT (pos.legal (m));

        Square org = org_sq (m);
        Square dst = dst_sq (m);
        Piece p    = pos[org];

        // Disambiguation if we have more then one piece with destination 'dst'
        // note that for pawns is not needed because starting file is explicit.

        Bitboard pinneds = pos.pinneds (pos.active ());

        Bitboard amb, b;
        amb = b = (attacks_bb (p, dst, pos.pieces ()) & pos.pieces (pos.active (), _ptype (p))) - org;
        while (b != U64 (0))
        {
            Square amb_org = pop_lsq (b);
            Move move = mk_move<NORMAL> (amb_org, dst);
            if (!pos.legal (move, pinneds))
            {
                amb -= amb_org;
            }
        }

        //if (!(amb)) return AMB_NONE;
        //if (!(amb & file_bb (org))) return AMB_RANK;
        //if (!(amb & rank_bb (org))) return AMB_FILE;
        //return AMB_SQR;

        if (amb != U64 (0))
        {
            if (!(amb & file_bb (org))) return AMB_RANK;
            if (!(amb & rank_bb (org))) return AMB_FILE;
            return AMB_SQR;
        }
        return AMB_NONE;
    }

    // move_from_can(can, pos) takes a position and a string representing a move in
    // simple coordinate algebraic notation and returns an equivalent legal move if any.
    Move move_from_can (const string &can, const Position &pos)
    {
        string scan = can;
        if (5 == scan.length ())
        {
            // promotion piece in lowercase
            if (isupper (uint8_t (scan[4])))
            {
                scan[4] = uint8_t (tolower (scan[4]));
            }
        }

        for (MoveList<LEGAL> itr (pos); *itr; ++itr)
        {
            Move m = *itr;
            if (scan == move_to_can (m, pos.chess960 ()))
            {
                return m;
            }
        }
        return MOVE_NONE;
    }

    // move_from_san(can, pos) takes a position and a string representing a move in
    // single algebraic notation and returns an equivalent legal move if any.
    Move move_from_san (const string &san, Position &pos)
    {
        for (MoveList<LEGAL> itr (pos); *itr; ++itr)
        {
            Move m = *itr;
            if (san == move_to_san (m, pos))
            {
                return m;
            }
        }
        return MOVE_NONE;
    }
    //Move move_from_lan (const string &lan, const Position &pos)
    //{
    //    return MOVE_NONE;
    //}
    //Move move_from_fan (const string &fan, const Position &pos)
    //{
    //    return MOVE_NONE;
    //}

    // move_to_can(m, c960) converts a move to a string in coordinate algebraic notation (g1f3, a7a8q, etc.).
    // The only special case is castling moves,
    //  - e1g1 notation in normal chess mode,
    //  - e1h1 notation in chess960 mode.
    // Internally castle moves are always coded as "king captures rook".
    const string move_to_can (Move m, bool c960)
    {
        if (MOVE_NONE == m) return "(none)";
        if (MOVE_NULL == m) return "(null)";
        //if (!_ok (m))       return "(xxxx)";

        Square org = org_sq (m);
        Square dst = dst_sq (m);
        MoveT mt   = mtype (m);
        if (!c960 && (CASTLE == mt)) dst = (((dst > org) ? F_G : F_C) | _rank (org));
        string can = to_string (org) + to_string (dst);
        if (PROMOTE == mt) can += PieceChar[(BLACK | prom_type (m))]; // lower case
        return can;
    }

    // move_to_san(m, pos) takes a position and a legal move as input
    // and returns its short algebraic notation representation.
    const string move_to_san (Move m, Position &pos)
    {
        if (MOVE_NONE == m) return "(none)";
        if (MOVE_NULL == m) return "(null)";
        //ASSERT (pos.legal (m));

        string san;

        Square org = org_sq (m);
        Square dst = dst_sq (m);
        Piece  p   = pos[org];
        PieceT pt  = _ptype (p);

        //switch (pt)
        //{
        //case PAWN:
        //    san = "";
        //    if (pos.capture (m))
        //    {
        //        san += to_char (_file (org));
        //        san += 'x';
        //    }
        //
        //    san += to_string (dst);
        //
        //    if (PROMOTE == mtype (m))
        //    {
        //        switch (prom_type (m))
        //        {
        //        case QUEN: san += "Q"; break;
        //        case ROOK: san += "R"; break;
        //        case BSHP: san += "B"; break;
        //        case NIHT: san += "N"; break;
        //        default: ASSERT (false); // "Wrong Promotion Piece"
        //        }
        //    }
        //    goto move_marker;
        //
        //case KING:
        //    if (CASTLE == mtype (m))
        //    {
        //        CSide cs = ((WHITE == pos.active ()) ?
        //            (dst == SQ_C1) ? CS_Q : (dst == SQ_G1) ? CS_K : CS_NO :
        //            (dst == SQ_C8) ? CS_Q : (dst == SQ_G8) ? CS_K : CS_NO);
        //
        //        switch (cs)
        //        {
        //        case CS_Q: san  = "O-";
        //        case CS_K: san += "O-O"; break;
        //        }
        //        goto move_marker;
        //    }
        //    // NOTE: no break
        //default:
        //    // piece notation
        //    san = PieceChar[pt];
        //
        //    break;
        //}
        //
        //switch (ambiguity (m, pos))
        //{
        //case AMB_NONE:                               break;
        //case AMB_RANK: san += to_char (_file (org)); break;
        //case AMB_FILE: san += to_char (_rank (org)); break;
        //case AMB_SQR:  san += to_string (org);       break;
        //default:       ASSERT (false);               break;
        //}
        //
        //if (pos.capture (m))
        //{
        //    san += 'x';
        //}
        //
        //san += to_string (dst);
        //// promote ????????
        //move_marker:
        //// Marker for check & checkmate
        //if (pos.gives_check (m, CheckInfo (pos)))
        //{
        //    StateInfo sinfo;
        //    Position p = pos;
        //    p.do_move (m, sinfo);
        //    uint8_t legalmove = MoveList<LEGAL> (p).size ();
        //    san += (legalmove ? '+' : '#');
        //}

        MoveT mt = mtype (m);
        switch (mt)
        {
        case CASTLE:
            san = (dst > org) ? "O-O" : "O-O-O";
            break;

        default:
        {
            bool capture = pos.capture (m);
            if (PAWN == pt)
            {
                if (capture) san = to_char (_file (org));
            }
            else
            {
                san = PieceChar[pt];
                // Disambiguation if we have more then one piece of type 'pt'
                // that can reach 'dst' with a legal move.
                switch (ambiguity (m, pos))
                {
                case AMB_NONE:                               break;
                case AMB_RANK: san += to_char (_file (org)); break;
                case AMB_FILE: san += to_char (_rank (org)); break;
                case AMB_SQR:  san += to_string (org);       break;
                //default:       ASSERT (false);               break;
                }
            }

            if (capture) san += 'x';
            san += to_string (dst);
            if (PROMOTE == mt && PAWN == pt) san += string ("=") + PieceChar[prom_type (m)];
        }
        }

        CheckInfo ci (pos);
        bool gives_check = pos.gives_check (m, ci);
        // Move marker for check & checkmate
        if (gives_check)
        {
            StateInfo si;
            pos.do_move (m, si, gives_check ? &ci : NULL);
            san += (MoveList<LEGAL> (pos).size () ? "+" : "#");
            pos.undo_move ();
        }

        return san;
    }

    // TODO::
    //// move_to_lan(m, pos) takes a position and a legal move as input
    //// and returns its long algebraic notation representation.
    //const string move_to_lan (Move m, Position &pos)
    //{
    //    string lan;
    //    return lan;
    //}
    //const std::string move_to_fan (Move m, Position &pos)
    //{
    //    string fan;
    //    return fan;
    //}

    // score_uci() converts a value to a string suitable
    // for use with the UCI protocol specifications:
    //
    // cp   <x>   The score x from the engine's point of view in centipawns.
    // mate <y>   Mate in y moves, not plies.
    //            If the engine is getting mated use negative values for y.
    const string score_uci (Value v, Value alpha, Value beta)
    {
        ostringstream os;

        if (abs (v) < VALUE_MATES_IN_MAX_PLY)
        {
            os << "cp " << 100 * int32_t (v) / int32_t (VALUE_MG_PAWN);
        }
        else
        {
            os << "mate " << int32_t (v > VALUE_ZERO ? (VALUE_MATE - v + 1) : -(VALUE_MATE + v)) / 2;
        }

        os << (beta <= v ? " lowerbound" : v <= alpha ? " upperbound" : "");

        return os.str ();
    }

    // pretty_pv() returns formated human-readable search information, typically to be
    // appended to the search log file. It uses the two helpers below to pretty
    // format the time and score respectively.
    const string pretty_pv (Position &pos, uint8_t depth, Value value, uint64_t msecs, const Move *pv)
    {
        const uint64_t K = 1000;
        const uint64_t M = 1000000;

        ostringstream os;

        os  << setw (3) << uint32_t (depth)
            << setw (8) << pretty_value (value)
            << setw (8) << pretty_time (msecs);

        if      (pos.game_nodes () < M)
        {
            os << setw (8) << pos.game_nodes () / 1 << "  ";
        }
        else if (pos.game_nodes () < K * M)
        {
            os << setw (7) << pos.game_nodes () / K << "K  ";
        }
        else
        {
            os << setw (7) << pos.game_nodes () / M << "M  ";
        }

        StateInfoStack states;

        string  padding = string (os.str ().length (), ' ');
        uint16_t length = padding.length ();

        const Move *m = pv;
        while (*m != MOVE_NONE)
        {
            string san = move_to_san (*m, pos);
            if (length + san.length () > 80)
            {
                os << "\n" + padding;
                length = padding.length ();
            }
            os << san << " ";
            length += san.length () + 1;
            states.push (StateInfo ());
            pos.do_move (*m, states.top ());
            ++m;
        }

        while (m != pv)
        {
            pos.undo_move ();
            --m;
        }

        return os.str ();
    }

}
