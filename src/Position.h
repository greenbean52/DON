#ifndef _POSITION_H_INC_
#define _POSITION_H_INC_

#include "BitBoard.h"
#include "Zobrist.h"

class Position;

// Check the validity of FEN string
extern bool _ok (const std::string &fen, bool c960 = false, bool full = true);

// StateInfo stores information needed to restore a Position object to its previous state
// when we retract a move. Whenever a move is made on the board (by calling do_move),
// a StateInfo object must be passed as a parameter.
//
//  - Castling-rights information for both sides.
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
    static const size_t Size;

    // ---Copied when making a move---
    Value  non_pawn_matl[CLR_NO];
    Score  psq_score     ;//= SCORE_ZERO;

    Key    matl_key      ;//= U64(0);   // Hash key of materials.
    Key    pawn_key      ;//= U64(0);   // Hash key of pawns.
    CastleRight castle_rights ;//= CR_NONE;  // Castling-rights information for both side.
    Square en_passant_sq ;//= SQ_NO;    // En-passant -> "In passing"
    u08    clock_ply     ;//= 0;        // Number of halfmoves clock since the last pawn advance or any capture.
                                        // Used to determine if a draw can be claimed under the clock-move rule.
    u08    null_ply      ;//= 0;
    // ---Not copied when making a move---
    Key    posi_key      ;//= U64(0);   // Hash key of position.
    Move   last_move     ;//= MOVE_NONE;// Move played on the previous position.
    PieceType capture_type;//= NONE;    // Piece type captured.
    Bitboard checkers    ;//= U64(0);   // Checkers bitboard.

    StateInfo *ptr ;//= nullptr;
};

// CheckInfo struct is initialized at constructor time
// and stores critical information used to detect if a move gives check.
//
//  - Checking squares from which the enemy king can be checked
//  - Pinned pieces.
//  - Check discoverer pieces.
//  - Enemy king square.
struct CheckInfo
{
public:
    Bitboard checking_bb[NONE];     // Checking squares from which the enemy king can be checked
    Bitboard pinneds    ;//= U64(0); // Pinned pieces
    Bitboard discoverers;//= U64(0); // Check discoverer pieces
    Square   king_sq    ;//= SQ_NO; // Enemy king square

    CheckInfo () = delete;
    explicit CheckInfo (const Position &pos);
};

namespace Threading {
    class Thread;
}

// The position data structure. A position consists of the following data:
//
// Board consits of data about piece placement
//  - 64-entry array of pieces, indexed by the square.
//  - Bitboards of each piece type.
//  - Bitboards of each color
//  - Bitboard of all occupied squares.
//  - List of squares for the pieces.
//  - Count of the pieces.
//  - ----------x-----------
//  - Color of side on move.
//  - Ply of the game.
//  - StateInfo object for the base status.
//  - StateInfo pointer for the current status.
//  - Information about the castling rights for both sides.
//  - Initial files of the kings and both pairs of rooks, also kings path
//    This is used to implement the Chess960 castling rules.
//  - Nodes visited during search.
//  - Chess 960 info
class Position
{

private:

    Piece    _board   [SQ_NO];  // Board for storing pieces.

    Bitboard _color_bb[CLR_NO];
    Bitboard _types_bb[MAX_PTYPE];

    SquareVector _piece_sq[CLR_NO][NONE];

    StateInfo  _ssi; // Startup state information object
    StateInfo *_psi; // Current state information pointer

    CastleRight _castle_mask[SQ_NO];
    Square      _castle_rook[CR_ALL];
    Bitboard    _castle_path[CR_ALL];
    Bitboard    _king_path  [CR_ALL];

    Color    _active;
    i16      _game_ply;
    u64      _game_nodes;

    bool     _chess960;

    Threading::Thread  *_thread;

    // ------------------------

    void  place_piece (Square s, Color c, PieceType pt);
    void  place_piece (Square s, Piece p);
    void remove_piece (Square s);
    void   move_piece (Square s1, Square s2);

    void set_castle (Color c, Square rook_org);

    bool can_en_passant (Square ep_sq) const;
    bool can_en_passant (File   ep_f ) const { return can_en_passant (ep_f | rel_rank (_active, R_6)); }

    template<bool Do>
    void do_castling (Square king_org, Square &king_dst, Square &rook_org, Square &rook_dst);

    template<PieceType PT>
    PieceType pick_least_val_att (Square dst, Bitboard stm_attackers, Bitboard &mocc, Bitboard &attackers) const;

    Bitboard check_blockers (Color piece_c, Color king_c) const;

    static const Score PSQ_Bonus[NONE][R_NO][F_NO/2];

public:
    static const size_t Size;

    static u08   DrawClockPly;
    static Score PSQ[CLR_NO][NONE][SQ_NO];

    static void initialize ();

    Position () = default; // To define the global object RootPos
    Position (const Position&) = delete;
    Position (const std::string &f, Threading::Thread *const th = nullptr, bool c960 = false, bool full = true)
    {
        if (!setup (f, th, c960, full))
        {
            clear ();
        }
    }
    Position (const Position &pos, Threading::Thread *const th)
    {
        *this = pos;
        _thread = th;
    }

    Position& operator= (const Position &pos); // To assign RootPos from UCI

    Piece    operator[] (Square s)      const;
    //Bitboard operator[] (Color  c)      const;
    //Bitboard operator[] (PieceType pt)  const;
    const SquareVector& operator[] (Piece p)  const;

    bool     empty  (Square s)  const;

    Bitboard pieces ()          const;
    Bitboard pieces (Color c)   const;
    Bitboard pieces (PieceType pt) const;
    Bitboard pieces (Color c, PieceType pt) const;
    Bitboard pieces (PieceType p1, PieceType p2) const;
    Bitboard pieces (Color c, PieceType p1, PieceType p2) const;

    template<PieceType PT>
    i32      count  ()          const;
    template<PieceType PT>
    i32      count  (Color c)   const;
    i32      count  (Color c, PieceType pt) const;

    template<PieceType PT>
    const SquareVector& squares (Color c) const;
    template<PieceType PT>
    Square square (Color c, i32 index = 0) const;

    CastleRight castle_rights () const;
    Square en_passant_sq () const;
    
    u08    clock_ply     () const;
    Move   last_move     () const;
    PieceType capture_type  () const;
    //Piece  capture_piece () const;  // Last piece captured
    Bitboard checkers    () const;

    Key matl_key      () const;
    Key pawn_key      () const;
    Key posi_key      () const;
    Key move_posi_key (Move m) const;

    Value non_pawn_material (Color c) const;

    Score psq_score () const;

    CastleRight can_castle (CastleRight cr) const;
    CastleRight can_castle (Color   c) const;

    Square   castle_rook (CastleRight cr) const;
    Bitboard castle_path (CastleRight cr) const;
    Bitboard king_path   (CastleRight cr) const;
    bool  castle_impeded (CastleRight cr) const;

    Color   active    () const;
    i16     game_ply  () const;
    i16     game_move () const;
    bool    chess960  () const;
    bool    draw      () const;
    bool    repeated  () const;

    u64   game_nodes ()  const;
    //void  game_nodes (u64 nodes);
    Phase game_phase ()  const;

    Threading::Thread* thread   ()  const;

    bool ok (i08 *failed_step = nullptr) const;

    Value see      (Move m) const;
    Value see_sign (Move m) const;
    
    Bitboard attackers_to (Square s, Color c, Bitboard occ) const;
    Bitboard attackers_to (Square s, Color c) const;
    Bitboard attackers_to (Square s, Bitboard occ) const;
    Bitboard attackers_to (Square s) const;

    Bitboard checkers    (Color c) const;
    Bitboard pinneds     (Color c) const;
    Bitboard discoverers (Color c) const;

    bool pseudo_legal  (Move m) const;
    bool legal         (Move m, Bitboard pinned) const;
    bool legal         (Move m) const;
    bool capture       (Move m) const;
    bool capture_or_promotion (Move m)  const;
    bool en_passant    (Move m)  const;
    bool gives_check   (Move m, const CheckInfo &ci) const;
    //bool gives_checkmate (Move m, const CheckInfo &ci)
    bool advanced_pawn_push (Move m)    const;
    //Piece moving_piece (Move m) const;

    bool passed_pawn  (Color c, Square s) const;
    bool bishops_pair (Color c) const;
    bool opposite_bishops ()    const;

    void clear ();

    bool setup (const std::string &f, Threading::Thread *const th = nullptr, bool c960 = false, bool full = true);

    Score compute_psq_score () const;
    Value compute_non_pawn_material (Color c) const;

    void do_move (Move m, StateInfo &nsi, bool gives_check);
    void do_move (const std::string &can, StateInfo &nsi);
    void undo_move ();
    void do_null_move (StateInfo &nsi);
    void undo_null_move ();

    void flip ();

    std::string fen (bool c960 = false, bool full = true) const;

    explicit operator std::string () const;

};

// -------------------------------

inline Piece         Position::operator[] (Square s)  const { return _board[s]; }
//inline Bitboard      Position::operator[] (Color  c)  const { return _color_bb[c];  }
//inline Bitboard      Position::operator[] (PieceType pt) const { return _types_bb[pt]; }
inline const SquareVector& Position::operator[] (Piece  p)  const { return _piece_sq[color (p)][ptype (p)]; }

inline bool     Position::empty  (Square s)  const { return _board[s] == NO_PIECE; }

inline Bitboard Position::pieces ()          const { return _types_bb[NONE]; }
inline Bitboard Position::pieces (Color c)   const { return _color_bb[c];  }
inline Bitboard Position::pieces (PieceType pt) const { return _types_bb[pt]; }
inline Bitboard Position::pieces (Color c,   PieceType pt) const { return _color_bb[c]&_types_bb[pt]; }
inline Bitboard Position::pieces (PieceType p1, PieceType p2) const { return _types_bb[p1]|_types_bb[p2]; }
inline Bitboard Position::pieces (Color c, PieceType p1, PieceType p2) const { return _color_bb[c]&(_types_bb[p1]|_types_bb[p2]); }

template<PieceType PT>
// Count specific piece
inline i32 Position::count          () const
{
    return i32(_piece_sq[WHITE][PT].size () + _piece_sq[BLACK][PT].size ());
}
template<>
// Count total pieces
inline i32 Position::count<NONE>    () const
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
inline i32 Position::count          (Color c) const { return i32(_piece_sq[c][PT].size ()); }
template<>
// Count total pieces of color
inline i32 Position::count<NONE>    (Color c) const
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
    assert(_piece_sq[c][PT].size () > index);
    return _piece_sq[c][PT][index];
}

// Castling rights for both side
inline CastleRight Position::castle_rights () const { return _psi->castle_rights; }
// Target square in algebraic notation. If there's no en passant target square is "-"
inline Square Position::en_passant_sq () const { return _psi->en_passant_sq; }
// Number of halfmoves clock since the last pawn advance or any capture.
// used to determine if a draw can be claimed under the clock-move rule.
inline u08    Position::clock_ply     () const { return _psi->clock_ply; }
inline Move   Position::last_move     () const { return _psi->last_move; }
inline PieceType Position::capture_type() const { return _psi->capture_type; }
//inline Piece  Position::capture_piece () const { return _psi->capture_type != NONE ? _active|_psi->capture_type : NO_PIECE; }
inline Bitboard Position::checkers    () const { return _psi->checkers; }

inline Key    Position::matl_key      () const { return _psi->matl_key; }
inline Key    Position::pawn_key      () const { return _psi->pawn_key; }
inline Key    Position::posi_key      () const { return _psi->posi_key; }
// move_posi_key() computes the new hash key after the given moven. Needed for speculative prefetch.
// It doesn't recognize special moves like castling, en-passant and promotions.
inline Key    Position::move_posi_key (Move m) const
{
    auto org = org_sq (m);
    auto dst = dst_sq (m);
    auto mpt = ptype (_board[org]);
    assert (!empty (org)
          && color (_board[org]) == _active
          && mpt != NONE);

    auto ppt = mtype (m) == PROMOTE
           && _board[org] == (_active|PAWN) ?
            promote (m) : mpt;
    auto cpt = mtype (m) == ENPASSANT
           && _board[org] == (_active|PAWN)
           &&  empty (dst)
           && _psi->en_passant_sq == dst ?
            PAWN : ptype (_board[dst]);
    
    return _psi->posi_key ^  Zob._.act_side
        ^  Zob._.piece_square[_active][mpt][org]
        ^  Zob._.piece_square[_active][ppt][dst]
        ^ (cpt != NONE ? Zob._.piece_square[~_active][cpt][dst] : U64(0));
}

inline Score  Position::psq_score () const { return _psi->psq_score; }
// Incremental piece-square evaluation
inline Value  Position::non_pawn_material (Color c) const { return _psi->non_pawn_matl[c]; }

inline CastleRight Position::can_castle (CastleRight cr) const { return _psi->castle_rights & cr; }
inline CastleRight Position::can_castle (Color   c) const { return _psi->castle_rights & mk_castle_right (c); }

inline Square   Position::castle_rook (CastleRight cr) const { return _castle_rook[cr]; }
inline Bitboard Position::castle_path (CastleRight cr) const { return _castle_path[cr]; }
inline Bitboard Position::king_path   (CastleRight cr) const { return _king_path[cr]; }

inline bool  Position::castle_impeded (CastleRight cr) const { return (_castle_path[cr] & _types_bb[NONE]) != U64(0); }
// Color of the side on move
inline Color Position::active   () const { return _active; }
// game_ply starts at 0, and is incremented after every move.
// game_ply  = max ((game_move - 1) * 2, 0) + (active == BLACK)
inline i16  Position::game_ply  () const { return _game_ply; }
// game_move starts at 1, and is incremented after BLACK's move.
// game_move = max ((game_ply - (active == BLACK)) / 2, 0) + 1
inline i16  Position::game_move () const { return i16(std::max ((_game_ply - (_active == BLACK)) / 2, 0) + 1); }
// Nodes visited
inline u64  Position::game_nodes() const { return _game_nodes; }
//inline void Position::game_nodes(u64 nodes){ _game_nodes = nodes; }
// game_phase() calculates the phase interpolating total
// non-pawn material between endgame and midgame limits.
inline Phase Position::game_phase () const
{
    return Phase(
        i32(std::max (std::min (_psi->non_pawn_matl[WHITE] + _psi->non_pawn_matl[BLACK], VALUE_MIDGAME), VALUE_ENDGAME) - VALUE_ENDGAME) * i32(PHASE_MIDGAME) /
        i32(VALUE_MIDGAME - VALUE_ENDGAME));
}

inline bool Position::chess960  () const { return _chess960; }
inline Threading::Thread* Position::thread () const { return _thread; }

// Attackers to the square 's' by color 'c' on occupancy 'occ'
inline Bitboard Position::attackers_to (Square s, Color c, Bitboard occ) const
{
    return((BitBoard::PawnAttacks[~c][s]    & _types_bb[PAWN])
        |  (BitBoard::PieceAttacks[NIHT][s] & _types_bb[NIHT])
        |  (BitBoard::attacks_bb<BSHP> (s, occ)&(_types_bb[BSHP]|_types_bb[QUEN]))
        |  (BitBoard::attacks_bb<ROOK> (s, occ)&(_types_bb[ROOK]|_types_bb[QUEN]))
        |  (BitBoard::PieceAttacks[KING][s] & _types_bb[KING])) & _color_bb[c];
}
// Attackers to the square 's' by color 'c'
inline Bitboard Position::attackers_to (Square s, Color c) const
{
    return attackers_to (s, c, _types_bb[NONE]);
}

// Attackers to the square 's' on occupancy 'occ'
inline Bitboard Position::attackers_to (Square s, Bitboard occ) const
{
    return (BitBoard::PawnAttacks[WHITE][s] & _types_bb[PAWN]&_color_bb[BLACK])
        |  (BitBoard::PawnAttacks[BLACK][s] & _types_bb[PAWN]&_color_bb[WHITE])
        |  (BitBoard::PieceAttacks[NIHT][s] & _types_bb[NIHT])
        |  (BitBoard::attacks_bb<BSHP> (s, occ)&(_types_bb[BSHP]|_types_bb[QUEN]))
        |  (BitBoard::attacks_bb<ROOK> (s, occ)&(_types_bb[ROOK]|_types_bb[QUEN]))
        |  (BitBoard::PieceAttacks[KING][s] & _types_bb[KING]);
}
// Attackers to the square 's'
inline Bitboard Position::attackers_to (Square s) const
{
    return attackers_to (s, _types_bb[NONE]);
}

// Checkers are enemy pieces that give the direct Check to friend King of color 'c'
inline Bitboard Position::checkers (Color c) const
{
    return attackers_to (_piece_sq[c][KING][0], ~c);
}
// Pinners => Only bishops, rooks, queens...  kings, knights, and pawns cannot pin.
// Pinneds => All except king, king must be immediately removed from check under all circumstances.
// Pinneds are friend pieces, that save the friend king from enemy pinners.
inline Bitboard Position::pinneds (Color c) const
{
    return check_blockers (c,  c); // blockers for own king
}
// Check discovers are candidate friend anti-sliders w.r.t piece behind it,
// that give the discover check to enemy king when moved.
inline Bitboard Position::discoverers (Color c) const
{
    return check_blockers (c, ~c); // blockers for opp king
}
inline bool Position::passed_pawn (Color c, Square s) const
{
    return ((_types_bb[PAWN]&_color_bb[~c]) & BitBoard::PawnPassSpan[c][s]) == U64(0);
}
// check the side has pair of opposite color bishops
inline bool Position::bishops_pair (Color c) const
{
    auto bishop_count = _piece_sq[c][BSHP].size ();
    if (bishop_count > 1)
    {
        for (u08 pc = 0; pc < bishop_count-1; ++pc)
        {
            if (opposite_colors (_piece_sq[c][BSHP][pc], _piece_sq[c][BSHP][pc+1])) return true;
        }
    }
    return false;
}
// check the opposite sides have opposite bishops
inline bool Position::opposite_bishops () const
{
    return _piece_sq[WHITE][BSHP].size () == 1
        && _piece_sq[BLACK][BSHP].size () == 1
        && opposite_colors (_piece_sq[WHITE][BSHP][0], _piece_sq[BLACK][BSHP][0]);
}
inline bool Position::legal         (Move m) const { return legal (m, pinneds (_active)); }
// capture(m) tests move is capture
inline bool Position::capture       (Move m) const
{
    // Castling is encoded as "king captures the rook"
    return ((mtype (m) == NORMAL || (mtype (m) == PROMOTE && _board[org_sq (m)] == (_active|PAWN))) && !empty (dst_sq (m))) || en_passant (m);
}
// capture_or_promotion(m) tests move is capture or promotion
inline bool Position::capture_or_promotion  (Move m) const
{
    return (mtype (m) == NORMAL && !empty (dst_sq (m))) || en_passant (m)
        || (mtype (m) == PROMOTE && _board[org_sq (m)] == (_active|PAWN));
}
inline bool Position::en_passant    (Move m) const
{
    return mtype (m) == ENPASSANT
        && _board[org_sq (m)] == (_active|PAWN)
        && empty (dst_sq (m))
        && _psi->en_passant_sq == dst_sq (m);
}
inline bool Position::advanced_pawn_push    (Move m) const
{
    return _board[org_sq (m)] == (_active|PAWN)
        && rel_rank (_active, org_sq (m)) > R_4;
}
//inline Piece Position::moving_piece (Move m) const { return _board[org_sq (m)]; }

inline void  Position:: place_piece (Square s, Color c, PieceType pt)
{
    //assert(empty (s));
    _board[s] = (c | pt);

    auto bb = BitBoard::Square_bb[s];
    _color_bb[c]    |= bb;
    _types_bb[pt]   |= bb;
    _types_bb[NONE] |= bb;

    auto &v = _piece_sq[c][pt];
    v.push_back (s);
}
inline void  Position:: place_piece (Square s, Piece p)
{
    assert(_ok (p));
    place_piece (s, color (p), ptype (p));
}
inline void  Position::remove_piece (Square s)
{
    //assert(!empty (s));
    auto c  = color (_board[s]);
    auto pt = ptype (_board[s]);
    //_board[s] = NO_PIECE; // Not needed, overwritten by the capturing one

    auto bb = ~BitBoard::Square_bb[s];
    _color_bb[c]    &= bb;
    _types_bb[pt]   &= bb;
    _types_bb[NONE] &= bb;

    auto &v = _piece_sq[c][pt];

    //v.erase (std::remove (v.begin (), v.end (), s), v.end ());

    if (v.size () > 1)
    {
        std::swap (*std::find (v.begin (), v.end (), s), v.back ());
    }
    v.pop_back ();
}
inline void  Position::  move_piece (Square s1, Square s2)
{
    //assert(!empty (s1));
    //assert( empty (s2));
    auto c  = color (_board[s1]);
    auto pt = ptype (_board[s1]);

    _board[s2] = _board[s1];
    _board[s1] = NO_PIECE;

    auto bb = BitBoard::Square_bb[s1] ^ BitBoard::Square_bb[s2];
    _color_bb[c]    ^= bb;
    _types_bb[pt]   ^= bb;
    _types_bb[NONE] ^= bb;

    auto &v = _piece_sq[c][pt];
    v[v.size () > 1 ? find (v.begin (), v.end (), s1) - v.begin () : 0] = s2;
}
// do_castling() is a helper used to do/undo a castling move.
// This is a bit tricky, especially in Chess960.
template<bool Do>
inline void Position::do_castling (Square king_org, Square &king_dst, Square &rook_org, Square &rook_dst)
{
    // Move the piece. The tricky Chess960 castle is handled earlier
    rook_org = king_dst; // castle is always encoded as "King captures friendly Rook"
    king_dst = rel_sq (_active, king_dst > king_org ? SQ_WKOO : SQ_WKOOO);
    rook_dst = rel_sq (_active, king_dst > king_org ? SQ_WROO : SQ_WROOO);
    // Remove both pieces first since squares could overlap in chess960
    remove_piece (Do ? king_org : king_dst);
    remove_piece (Do ? rook_org : rook_dst);
    _board[Do ? king_org : king_dst] = _board[Do ? rook_org : rook_dst] = NO_PIECE; // Not done by remove_piece()
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
    Color own = pos.active ();
    Color opp = ~own;

    king_sq = pos.square<KING> (opp);
    pinneds = pos.pinneds (own);
    discoverers = pos.discoverers (own);

    checking_bb[PAWN] = BitBoard::PawnAttacks[opp][king_sq];
    checking_bb[NIHT] = BitBoard::PieceAttacks[NIHT][king_sq];
    checking_bb[BSHP] = BitBoard::attacks_bb<BSHP> (king_sq, pos.pieces ());
    checking_bb[ROOK] = BitBoard::attacks_bb<ROOK> (king_sq, pos.pieces ());
    checking_bb[QUEN] = checking_bb[BSHP] | checking_bb[ROOK];
    checking_bb[KING] = U64(0);
}

#endif // _POSITION_H_INC_
