#ifndef _POSITION_H_INC_
#define _POSITION_H_INC_

#include <deque>
#include <memory>

#include "BitBoard.h"
#include "Zobrist.h"

class Position;
using namespace BitBoard;

// StateInfo stores information needed to restore a Position object to its previous state
// when we retract a move. Whenever a move is made on the board (by calling do_move),
// a StateInfo object must be passed as a parameter.
//
//  - Castling-rights information.
//  - En-passant square (SQ_NO if no en passant capture is possible).
//  - Counter (clock) for detecting 50 move rule draws.
//  - Hash key of the material situation.
//  - Hash key of the pawn structure.
//  - Hash key of the position.
//  - Move played on the last position.
//  - Piece type captured on last position.
//  - Bitboard of all checking pieces.
//  - Pointer to previous StateInfo. 
//  - Hash keys for all previous positions in the game for detecting repetition draws.
struct StateInfo
{
public:
    // Copied when making a move
    Key         matl_key;       // Hash key of materials.
    Key         pawn_key;       // Hash key of pawns.

    Value       non_pawn_matl[CLR_NO];
    Score       psq_score;

    CastleRight castle_rights;  // Castling-rights information.
    Square      en_passant_sq;  // En-passant -> "In passing"
    u08         clock_ply;      // Number of halfmoves clock since the last pawn advance or any capture.
                                // Used to determine if a draw can be claimed under the clock-move rule.
    u08         null_ply;   

    // Not copied when making a move
    Key         posi_key;       // Hash key of position.
    Move        last_move;      // Move played on the previous position.
    PieceType   capture_type;   // Piece type captured.
    Bitboard    checkers;       // Checkers bitboard.

    StateInfo   *ptr;           // Previous StateInfo.

    StateInfo () = default;
};

typedef std::deque<StateInfo>       StateList;
typedef std::unique_ptr<StateList>  StateListPtr;

// CheckInfo struct stores information used to detect if a move gives check.
struct CheckInfo
{
public:
    Bitboard checking_bb[NONE]; // Checking squares from which the enemy king can be checked.
    Bitboard abs_pinneds;       // Absolute pinneds pieces.
    Bitboard dsc_checkers;      // Discovered checkers pieces.
    Square   king_sq;           // Enemy king square.

    CheckInfo () = delete;
    explicit CheckInfo (const Position &pos);
};

namespace Threading {
    class Thread;
}
using namespace Threading;

#if !defined(NDEBUG)
// Check the validity of FEN string
extern bool _ok (const std::string &fen, bool full = true);
#endif

// Position class stores information regarding the board representation:
//  - 64-entry array of pieces, indexed by the square.
//  - Bitboards of each piece type.
//  - Bitboards of each color
//  - Bitboard of all occupied squares.
//  - List of squares for the pieces.
//  - Count of the pieces.
//  - ----------x-----------
//  - Color of side on move.
//  - Ply of the game.
//  - Nodes visited during search.
//  - StateInfo object for the base status.
//  - StateInfo pointer for the current status.
//  - Information about the castling rights.
//  - Initial files of both pairs of rooks, castle path and kings path, this is used to implement the Chess960 castling rules.
class Position
{
private:
    Piece       _board[SQ_NO];  // Board for storing pieces.

    Bitboard    _color_bb[CLR_NO];
    Bitboard    _types_bb[MAX_PTYPE];

    SquareVector _piece_sq[CLR_NO][NONE];

    CastleRight _castle_mask[SQ_NO];
    Square      _castle_rook[CR_NO];
    Bitboard    _castle_path[CR_NO];
    Bitboard    _king_path  [CR_NO];

    Color       _active;
    i16         _ply;
    u64         _nodes;

    StateInfo   *_si; // Current state information pointer
    Thread      *_thread;

    // ------------------------

    void place_piece (Square s, Color c, PieceType pt);
    void place_piece (Square s, Piece p);
    void remove_piece (Square s);
    void move_piece (Square s1, Square s2);

    void set_castle (Color c, Square rook_org);

    bool can_en_passant (Square ep_sq) const;
    bool can_en_passant (File ep_f) const;

    template<bool Do>
    void do_castling (Square king_org, Square &king_dst, Square &rook_org, Square &rook_dst);

    template<PieceType PT>
    PieceType pick_least_val_att (Square dst, Bitboard stm_attackers, Bitboard &mocc, Bitboard &attackers) const;

public:
    static u08  DrawClockPly;
    static bool Chess960;

    Position () = default;
    Position (const Position&) = delete;
    Position& operator= (const Position &pos) = delete;

    Piece operator[] (Square s) const;
    //Bitboard operator[] (Color  c) const;
    //Bitboard operator[] (PieceType pt) const;
    const SquareVector& operator[] (Piece p)  const;

    bool empty  (Square s)  const;

    Bitboard pieces () const;
    Bitboard pieces (Color c) const;
    Bitboard pieces (PieceType pt) const;
    Bitboard pieces (Color c, PieceType pt) const;
    Bitboard pieces (PieceType p1, PieceType p2) const;
    Bitboard pieces (Color c, PieceType p1, PieceType p2) const;

    template<PieceType PT>
    i32      count  () const;
    template<PieceType PT>
    i32      count  (Color c) const;
    i32      count  (Color c, PieceType pt) const;

    template<PieceType PT>
    const SquareVector& squares (Color c) const;
    template<PieceType PT>
    Square square (Color c, i32 index = 0) const;

    CastleRight castle_rights () const;
    Square en_passant_sq () const;

    u08  clock_ply () const;
    Move last_move () const;
    PieceType capture_type () const;
    //Piece  capture_piece () const;  // Last piece captured
    Bitboard checkers () const;

    Key matl_key () const;
    Key pawn_key () const;
    Key posi_key () const;
    Key poly_key () const;
    Key move_posi_key (Move m) const;

    Value non_pawn_material (Color c) const;

    Score psq_score () const;

    CastleRight can_castle (Color c) const;
    CastleRight can_castle (CastleRight cr) const;

    Square   castle_rook (CastleRight cr) const;
    Bitboard castle_path (CastleRight cr) const;
    Bitboard king_path   (CastleRight cr) const;
    bool  castle_impeded (CastleRight cr) const;

    Color   active   () const;
    i16     ply      () const;
    u64     nodes    ()  const;

    i16     move_num () const;
    bool    draw     () const;
    bool    repeated () const;
    Phase   phase    ()  const;

    Thread* thread   ()  const;
#if !defined(NDEBUG)
    bool ok (i08 *failed_step = nullptr) const;
#endif

    Value see (Move m) const;
    Value see_sign (Move m) const;

    Bitboard attackers_to (Square s, Color c, Bitboard occ) const;
    Bitboard attackers_to (Square s, Color c) const;
    Bitboard attackers_to (Square s, Bitboard occ) const;
    Bitboard attackers_to (Square s) const;
    Bitboard checkers    (Color c) const;

    Bitboard slider_blockers (Square s, Bitboard sliders, Bitboard target) const;
    Bitboard abs_pinneds (Color c) const;
    Bitboard dsc_checkers (Color c) const;

    bool pseudo_legal   (Move m) const;
    bool legal          (Move m, Bitboard abs_pinned) const;
    bool legal          (Move m) const;
    bool capture        (Move m) const;
    bool promotion (Move m) const;
    bool capture_or_promotion (Move m) const;
    bool en_passant     (Move m) const;
    bool gives_check    (Move m, const CheckInfo &ci) const;
    //bool gives_checkmate (Move m, const CheckInfo &ci) const;

    bool pawn_passed_at (Color c, Square s) const;
    bool bishops_pair (Color c) const;
    bool opposite_bishops ()    const;

    Value compute_non_pawn_material (Color c) const;

    void clear ();

    Position& setup (const std::string &ff, StateInfo &si, Thread *const th = nullptr, bool full = true);
    Position& setup (const std::string &code, StateInfo &si, Color c);

    void do_move (Move m, StateInfo &si, bool gives_check);
    void do_move (const std::string &can, StateInfo &si);
    void undo_move ();
    void do_null_move (StateInfo &si);
    void undo_null_move ();

    void flip ();

    std::string fen (bool full = true) const;

    explicit operator std::string () const;

};

// -------------------------------

inline Piece Position::operator[] (Square s) const { return _board[s]; }
//inline Bitboard Position::operator[] (Color  c) const { return _color_bb[c];  }
//inline Bitboard Position::operator[] (PieceType pt) const { return _types_bb[pt]; }
inline const SquareVector& Position::operator[] (Piece  p) const { return _piece_sq[color (p)][ptype (p)]; }

inline bool Position::empty  (Square s)  const { return _board[s] == NO_PIECE; }

inline Bitboard Position::pieces () const { return _types_bb[NONE]; }
inline Bitboard Position::pieces (Color c) const { return _color_bb[c]; }
inline Bitboard Position::pieces (PieceType pt) const { return _types_bb[pt]; }
inline Bitboard Position::pieces (Color c,   PieceType pt) const { return _color_bb[c]&_types_bb[pt]; }
inline Bitboard Position::pieces (PieceType p1, PieceType p2) const { return _types_bb[p1]|_types_bb[p2]; }
inline Bitboard Position::pieces (Color c, PieceType p1, PieceType p2) const { return _color_bb[c]&(_types_bb[p1]|_types_bb[p2]); }

template<PieceType PT>
// Count specific piece
inline i32 Position::count () const { return i32(_piece_sq[WHITE][PT].size () + _piece_sq[BLACK][PT].size ()); }
template<>
// Count total pieces
inline i32 Position::count<NONE> () const
{
    return i32(_piece_sq[WHITE][PAWN].size () + _piece_sq[BLACK][PAWN].size ()
             + _piece_sq[WHITE][NIHT].size () + _piece_sq[BLACK][NIHT].size ()
             + _piece_sq[WHITE][BSHP].size () + _piece_sq[BLACK][BSHP].size ()
             + _piece_sq[WHITE][ROOK].size () + _piece_sq[BLACK][ROOK].size ()
             + _piece_sq[WHITE][QUEN].size () + _piece_sq[BLACK][QUEN].size ()
             + _piece_sq[WHITE][KING].size () + _piece_sq[BLACK][KING].size ());
}
template<>
// Count non-pawn pieces
inline i32 Position::count<NONPAWN> () const
{
    return i32(_piece_sq[WHITE][NIHT].size () + _piece_sq[BLACK][NIHT].size ()
             + _piece_sq[WHITE][BSHP].size () + _piece_sq[BLACK][BSHP].size ()
             + _piece_sq[WHITE][ROOK].size () + _piece_sq[BLACK][ROOK].size ()
             + _piece_sq[WHITE][QUEN].size () + _piece_sq[BLACK][QUEN].size ());
}
template<PieceType PT>
// Count specific piece of color
inline i32 Position::count (Color c) const { return i32(_piece_sq[c][PT].size ()); }
template<>
// Count total pieces of color
inline i32 Position::count<NONE> (Color c) const
{
    return i32(_piece_sq[c][PAWN].size ()
             + _piece_sq[c][NIHT].size ()
             + _piece_sq[c][BSHP].size ()
             + _piece_sq[c][ROOK].size ()
             + _piece_sq[c][QUEN].size ()
             + _piece_sq[c][KING].size ());
}
template<>
// Count non-pawn pieces of color
inline i32 Position::count<NONPAWN> (Color c) const
{
    return i32(_piece_sq[c][NIHT].size ()
             + _piece_sq[c][BSHP].size ()
             + _piece_sq[c][ROOK].size ()
             + _piece_sq[c][QUEN].size ());
}
inline i32 Position::count (Color c, PieceType pt) const { return i32(_piece_sq[c][pt].size ()); }

template<PieceType PT>
inline const SquareVector& Position::squares (Color c) const { return _piece_sq[c][PT]; }
template<PieceType PT>
inline Square Position::square (Color c, i32 index) const
{
    assert(i32(_piece_sq[c][PT].size ()) > index);
    return _piece_sq[c][PT][index];
}

// Castling rights
inline CastleRight Position::castle_rights () const { return _si->castle_rights; }
// Target square in algebraic notation. If there's no en passant target square is "-"
inline Square Position::en_passant_sq () const { return _si->en_passant_sq; }
// Number of halfmoves clock since the last pawn advance or any capture.
// used to determine if a draw can be claimed under the clock-move rule.
inline u08 Position::clock_ply () const { return _si->clock_ply; }
inline Move Position::last_move () const { return _si->last_move; }
inline PieceType Position::capture_type () const { return _si->capture_type; }
//inline Piece  Position::capture_piece () const { return _ok (_si->capture_type) ? _active|_si->capture_type : NO_PIECE; }
inline Bitboard Position::checkers () const { return _si->checkers; }

inline Key Position::matl_key () const { return _si->matl_key; }
inline Key Position::pawn_key () const { return _si->pawn_key; }
inline Key Position::posi_key () const { return _si->posi_key; }
inline Key Position::poly_key () const { return PolyZob.compute_posi_key (*this); }
// Computes the new hash key after the given moven. Needed for speculative prefetch.
// It doesn't recognize special moves like castling, en-passant and promotions.
inline Key Position::move_posi_key (Move m) const
{
    auto org = org_sq (m);
    auto dst = dst_sq (m);
    auto mpt = ptype (_board[org]);
    assert(!empty (org)
          && color (_board[org]) == _active
          && _ok (mpt));

    auto ppt = promotion (m) ? promote (m) : mpt;
    auto cpt = en_passant (m) ? PAWN : ptype (_board[dst]);
    Key key = _si->posi_key ^ Zob.active_color
        ^ Zob.piece_square[_active][ppt][dst]
        ^ Zob.piece_square[_active][mpt][org];
    if (_ok (cpt))
    {
        key ^= Zob.piece_square[~_active][cpt][en_passant (m) ? dst - pawn_push (_active) : dst];
    }
    return key;
}

inline Score  Position::psq_score () const { return _si->psq_score; }
// Incremental piece-square evaluation
inline Value  Position::non_pawn_material (Color c) const { return _si->non_pawn_matl[c]; }

inline CastleRight Position::can_castle (Color c) const { return _si->castle_rights & mk_castle_right (c); }
inline CastleRight Position::can_castle (CastleRight cr) const { return _si->castle_rights & cr; }

inline Square   Position::castle_rook (CastleRight cr) const { return _castle_rook[cr]; }
inline Bitboard Position::castle_path (CastleRight cr) const { return _castle_path[cr]; }
inline Bitboard Position::king_path   (CastleRight cr) const { return _king_path[cr]; }

inline bool  Position::castle_impeded (CastleRight cr) const { return (_castle_path[cr] & pieces ()) != 0; }
// Color of the side on move
inline Color Position::active  () const { return _active; }
// ply starts at 0, and is incremented after every move.
// ply  = max ((move_num - 1) * 2, 0) + (active == BLACK)
inline i16  Position::ply () const { return _ply; }
// move_num starts at 1, and is incremented after BLACK's move.
// move_num = max ((game_ply - (active == BLACK)) / 2, 0) + 1
inline i16  Position::move_num () const { return i16(std::max ((_ply - (_active == BLACK ? 1 : 0)) / 2, 0) + 1); }
// Nodes searched
inline u64  Position::nodes () const { return _nodes; }
// Calculates the phase interpolating total non-pawn material between endgame and midgame limits.
inline Phase Position::phase () const
{
    return Phase(
        i32(std::max (std::min (_si->non_pawn_matl[WHITE] + _si->non_pawn_matl[BLACK], VALUE_MIDGAME), VALUE_ENDGAME) - VALUE_ENDGAME) * i32(PHASE_MIDGAME) /
        i32(VALUE_MIDGAME - VALUE_ENDGAME));
}

inline Thread* Position::thread () const { return _thread; }

// Attackers to the square 's' by color 'c' on occupancy 'occ'
inline Bitboard Position::attackers_to (Square s, Color c, Bitboard occ) const
{
    return (  (PawnAttacks[~c][s]        & pieces (PAWN))
            | (PieceAttacks[NIHT][s]     & pieces (NIHT))
            | (attacks_bb<BSHP> (s, occ) & pieces (BSHP, QUEN))
            | (attacks_bb<ROOK> (s, occ) & pieces (ROOK, QUEN))
            | (PieceAttacks[KING][s]     & pieces (KING))) & pieces (c);
}
// Attackers to the square 's' by color 'c'
inline Bitboard Position::attackers_to (Square s, Color c) const
{
    return attackers_to (s, c, pieces ());
}

// Attackers to the square 's' on occupancy 'occ'
inline Bitboard Position::attackers_to (Square s, Bitboard occ) const
{
    return (  (PawnAttacks[WHITE][s]     & pieces (BLACK, PAWN))
            | (PawnAttacks[BLACK][s]     & pieces (WHITE, PAWN))
            | (PieceAttacks[NIHT][s]     & pieces (NIHT))
            | (attacks_bb<BSHP> (s, occ) & pieces (BSHP, QUEN))
            | (attacks_bb<ROOK> (s, occ) & pieces (ROOK, QUEN))
            | (PieceAttacks[KING][s]     & pieces (KING)));
}
// Attackers to the square 's'
inline Bitboard Position::attackers_to (Square s) const
{
    return attackers_to (s, pieces ());
}
// Checkers are enemy pieces that give the direct check to friend King of color 'c'
inline Bitboard Position::checkers (Color c) const
{
    return attackers_to (square<KING> (c), ~c);
}

// Absolute pinneds are friend pieces, that save the friend king from enemy checkers.
inline Bitboard Position::abs_pinneds (Color c) const
{
    return slider_blockers (square<KING> ( c), pieces (~c), pieces (c));
}
// Discovered checkers are friend pieces, that give the discover check to enemy king when moved.
inline Bitboard Position::dsc_checkers (Color c) const
{
    return slider_blockers (square<KING> (~c), pieces ( c), pieces (c));
}
// Pawn passed at the given square
inline bool Position::pawn_passed_at (Color c, Square s) const
{
    return (pieces (~c, PAWN) & pawn_pass_span (c, s)) == 0;
}
// Check the side has pair of opposite color bishops
inline bool Position::bishops_pair (Color c) const
{
    for (i32 pc = 1; pc < count<BSHP> (c); ++pc)
    {
        if (opposite_colors (square<BSHP> (c, pc-1), square<BSHP> (c, pc)))
        {
            return true;
        }
    }
    return false;
}
// Check the opposite sides have opposite bishops
inline bool Position::opposite_bishops () const
{
    return count<BSHP> (WHITE) == 1
        && count<BSHP> (BLACK) == 1
        && opposite_colors (square<BSHP> (WHITE), square<BSHP> (BLACK));
}
inline bool Position::legal (Move m) const { return legal (m, abs_pinneds (_active)); }
// Checks move is capture
inline bool Position::capture (Move m) const
{
    // Castling is encoded as "king captures the rook"
    return ((mtype (m) == NORMAL || promotion (m)) && (pieces (~_active) & dst_sq (m)) != 0)
        || en_passant (m);
}
// Checks move is promotion
inline bool Position::promotion (Move m) const
{
    return mtype (m) == PROMOTE
        && _board[org_sq (m)] == (_active|PAWN)
        && rel_rank (_active, dst_sq (m)) == R_8;
}
// Checks move is capture or promotion
inline bool Position::capture_or_promotion (Move m) const
{
    return (mtype (m) == NORMAL && (pieces (~_active) & dst_sq (m)) != 0)
        || en_passant (m)
        || promotion (m);
}
// Checks move is en-passant
inline bool Position::en_passant (Move m) const
{
    return mtype (m) == ENPASSANT
        && _board[org_sq (m)] == (_active|PAWN)
        && _si->en_passant_sq == dst_sq (m)
        && empty (dst_sq (m));
}

inline void  Position::place_piece (Square s, Color c, PieceType pt)
{
    _board[s] = (c|pt);

    Bitboard bb = square_bb (s);
    _color_bb[c]    |= bb;
    _types_bb[pt]   |= bb;
    _types_bb[NONE] |= bb;

    auto &v = _piece_sq[c][pt];
    v.push_back (s);
}
inline void  Position::place_piece (Square s, Piece p)
{
    assert(_ok (p));
    place_piece (s, color (p), ptype (p));
}
inline void  Position::remove_piece (Square s)
{
    auto c  = color (_board[s]);
    auto pt = ptype (_board[s]);
    //_board[s] = NO_PIECE; // Not needed, overwritten by the capturing one

    Bitboard bb = ~square_bb (s);
    _color_bb[c]    &= bb;
    _types_bb[pt]   &= bb;
    _types_bb[NONE] &= bb;

    auto &v = _piece_sq[c][pt];
    assert(!v.empty ());
    if (v.size () > 1)
    {
        std::swap (*std::find (v.begin (), v.end (), s), v.back ());
    }
    v.pop_back ();
}
inline void  Position::move_piece (Square s1, Square s2)
{
    auto c  = color (_board[s1]);
    auto pt = ptype (_board[s1]);

    _board[s2] = _board[s1];
    _board[s1] = NO_PIECE;

    Bitboard bb =
          square_bb (s1)
        ^ square_bb (s2);
    _color_bb[c]    ^= bb;
    _types_bb[pt]   ^= bb;
    _types_bb[NONE] ^= bb;

    auto &v = _piece_sq[c][pt];
    assert(!v.empty ());
    v[v.size () > 1 ? std::find (v.begin (), v.end (), s1) - v.begin () : 0] = s2;
}
// do_castling() is a helper used to do/undo a castling move.
// This is a bit tricky, especially in Chess960.
template<bool Do>
inline void Position::do_castling (Square king_org, Square &king_dst, Square &rook_org, Square &rook_dst)
{
    // Move the piece. The tricky Chess960 castle is handled earlier
    rook_org = king_dst; // castle is always encoded as "King captures friendly Rook"
    king_dst = rel_sq (_active, king_dst > king_org ? SQ_G1 : SQ_C1);
    rook_dst = rel_sq (_active, king_dst > king_org ? SQ_F1 : SQ_D1);
    // Remove both pieces first since squares could overlap in chess960
    remove_piece (Do ? king_org : king_dst);
    remove_piece (Do ? rook_org : rook_dst);
    _board[Do ? king_org : king_dst] =
    _board[Do ? rook_org : rook_dst] = NO_PIECE; // Not done by remove_piece()
    place_piece (Do ? king_dst : king_org, _active, KING);
    place_piece (Do ? rook_dst : rook_org, _active, ROOK);
}

template<class CharT, class Traits>
inline std::basic_ostream<CharT, Traits>&
operator<< (std::basic_ostream<CharT, Traits> &os, const Position &pos)
{
    os << std::string(pos);
    return os;
}

// ----------------------------------------------
// CheckInfo constructor
inline CheckInfo::CheckInfo (const Position &pos)
{
    king_sq      = pos.square<KING> (~pos.active ());
    abs_pinneds  = pos.abs_pinneds (pos.active ());
    dsc_checkers = pos.dsc_checkers (pos.active ());
    
    checking_bb[PAWN] = PawnAttacks[~pos.active ()][king_sq];
    checking_bb[NIHT] = PieceAttacks[NIHT][king_sq];
    checking_bb[BSHP] = attacks_bb<BSHP> (king_sq, pos.pieces ());
    checking_bb[ROOK] = attacks_bb<ROOK> (king_sq, pos.pieces ());
    checking_bb[QUEN] = checking_bb[BSHP] | checking_bb[ROOK];
    checking_bb[KING] = 0;
}

#endif // _POSITION_H_INC_
