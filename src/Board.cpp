#include "Board.h"
//#include <sstream>
//#include <iomanip>
#include <algorithm>
#include "BitBoard.h"
#include "BitCount.h"
#include "BitScan.h"

using namespace BitBoard;

void Board::clear ()
{
    //for (Square s = SQ_A1; s <= SQ_H8; ++s) _piece_arr[s] = PS_NO;
    //for (Color c = WHITE; c <= BLACK; ++c) _color_bb[c] = 0;
    //for (PType t = PAWN; t <= PT_NO; ++t) _types_bb[t] = 0;

    std::fill_n (_piece_arr, sizeof (_piece_arr) / sizeof (*_piece_arr), PS_NO);
    std::fill_n (_color_bb, sizeof (_color_bb) / sizeof (*_color_bb), 0);
    std::fill_n (_types_bb, sizeof (_types_bb) / sizeof (*_types_bb), 0);

    for (Color c = WHITE; c <= BLACK; ++c)
    {
        for (PType t = PAWN; t <= KING; ++t)
        {
            _piece_list[c][t].clear ();
        }
    }

}
//void Board::reset ()
//{
//    clear ();
//
//    place_piece (SQ_A1, W_ROOK);
//    place_piece (SQ_B1, W_NIHT);
//    place_piece (SQ_C1, W_BSHP);
//    place_piece (SQ_D1, W_QUEN);
//    place_piece (SQ_E1, W_KING);
//    place_piece (SQ_F1, W_BSHP);
//    place_piece (SQ_G1, W_NIHT);
//    place_piece (SQ_H1, W_ROOK);
//    place_piece (SQ_A2, W_PAWN);
//    place_piece (SQ_B2, W_PAWN);
//    place_piece (SQ_C2, W_PAWN);
//    place_piece (SQ_D2, W_PAWN);
//    place_piece (SQ_E2, W_PAWN);
//    place_piece (SQ_F2, W_PAWN);
//    place_piece (SQ_G2, W_PAWN);
//    place_piece (SQ_H2, W_PAWN);
//
//    place_piece (SQ_A7, B_PAWN);
//    place_piece (SQ_B7, B_PAWN);
//    place_piece (SQ_C7, B_PAWN);
//    place_piece (SQ_D7, B_PAWN);
//    place_piece (SQ_E7, B_PAWN);
//    place_piece (SQ_F7, B_PAWN);
//    place_piece (SQ_G7, B_PAWN);
//    place_piece (SQ_H7, B_PAWN);
//    place_piece (SQ_A8, B_ROOK);
//    place_piece (SQ_B8, B_NIHT);
//    place_piece (SQ_C8, B_BSHP);
//    place_piece (SQ_D8, B_QUEN);
//    place_piece (SQ_E8, B_KING);
//    place_piece (SQ_F8, B_BSHP);
//    place_piece (SQ_G8, B_NIHT);
//    place_piece (SQ_H8, B_ROOK);
//}

void Board::place_piece (Square s, Color c, PType t)
{
    //if (PS_NO != _piece_arr[s]) return;
    _piece_arr[s] = c | t;
    _color_bb[c] += s;
    _types_bb[t] += s;
    _types_bb[PT_NO] += s;
    // Update piece list, put piece at [s] index
    _piece_list[c][t].emplace_back (s);
}
void Board::place_piece (Square s, Piece p)
{
    place_piece (s, _color (p), _ptype (p));
}
Piece Board::remove_piece (Square s)
{
    Piece p = _piece_arr[s];
    ASSERT (PS_NO != p);
    Color c = _color (p);
    PType t = _ptype (p);

    SquareList &lst_sq  = _piece_list[c][t];
    uint8_t ps_count    = lst_sq.size ();

    ASSERT (0 < ps_count);
    if (0 >= ps_count) return PS_NO;

    _piece_arr[s] = PS_NO;

    _color_bb[c] -= s;
    _types_bb[t] -= s;
    _types_bb[PT_NO] -= s;

    // Update piece list, remove piece at [s] index and shrink the list.
    lst_sq.erase (std::remove (lst_sq.begin(), lst_sq.end(), s), lst_sq.end());

    return p;
}
Piece Board::move_piece (Square s1, Square s2)
{
    if (s1 == s2) return _piece_arr[s1];

    Piece mp = _piece_arr[s1];
    //if (!_ok (mp)) return PS_NO;
    //if (PS_NO != _piece_arr[s2]) return PS_NO;

    Color mc = _color (mp);
    PType mt = _ptype (mp);

    _piece_arr[s1] = PS_NO;
    _piece_arr[s2] = mp;

    _color_bb[mc] -= s1;
    _types_bb[mt] -= s1;
    _types_bb[PT_NO] -= s1;

    _color_bb[mc] += s2;
    _types_bb[mt] += s2;
    _types_bb[PT_NO] += s2;

    SquareList &lst_sq = _piece_list[mc][mt];
    std::replace (lst_sq.begin (), lst_sq.end (), s1, s2);

    return mp;
}

Board::operator std::string() const
{
    std::string brd;
    const std::string dots = " +---+---+---+---+---+---+---+---+\n";
    const std::string row_1 = "| . |   | . |   | . |   | . |   |\n" + dots;
    const std::string row_2 = "|   | . |   | . |   | . |   | . |\n" + dots;
    const size_t len_row = row_1.length () + 1;
    brd = dots;
    for (Rank r = R_8; r >= R_1; --r)
    {
        brd += to_char (r) + ((r % 2) ? row_1 : row_2);
    }
    for (File f = F_A; f <= F_H; ++f)
    {
        brd += "   ";
        brd += to_char (f, false);
    }

    Bitboard occ = pieces ();
    while (occ)
    {
        Square s = pop_lsb (occ);
        int8_t r = _rank (s);
        int8_t f = _file (s);
        brd[3 + size_t (len_row * (7.5 - r)) + 4 * f] = to_char (_piece_arr[s]);
    }
    return brd;
}

// Board consistency check, for debugging
bool Board::ok (int8_t *failed_step) const
{
    int8_t step_dummy, *step = failed_step ? failed_step : &step_dummy;

    // What features of the board should be verified?
    const bool debug_all = true;
    const bool debug_king_count  = debug_all || false;
    const bool debug_piece_count = debug_all || false;
    const bool debug_bitboards   = debug_all || false;
    const bool debug_piece_list  = debug_all || false;

    *step = 0;

    if (++(*step), W_KING != _piece_arr[king_sq (WHITE)]) return false;
    if (++(*step), B_KING != _piece_arr[king_sq (BLACK)]) return false;

    if (++(*step), debug_king_count)
    {
        uint8_t king_count[CLR_NO] = {};
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
        {
            Piece p = _piece_arr[s];
            if (KING == _ptype (p)) ++king_count[_color (p)];
        }
        for (Color c = WHITE; c <= BLACK; ++c)
        {
            if (1 != king_count[c]) return false;
            if (piece_count<KING> (c) != pop_count<FULL> (pieces (c, KING))) return false;
        }
    }

    if (++(*step), debug_piece_count)
    {
        if (pop_count<FULL> (pieces ()) > 32) return false;
        if (piece_count () > 32) return false;
        if (piece_count () != pop_count<FULL> (pieces ())) return false;

        for (Color c = WHITE; c <= BLACK; ++c)
        {
            for (PType t = PAWN; t <= KING; ++t)
            {
                if (piece_count (c, t) != pop_count<FULL> (pieces (c, t))) return false;
            }
        }
    }

    if (++(*step), debug_bitboards)
    {
        for (Color c = WHITE; c <= BLACK; ++c)
        {
            Bitboard colors = pieces (c);

            if (pop_count<FULL> (colors) > 16) return false; // Too many Piece of color

            // check if the number of Pawns plus the number of
            // extra Queens, Rooks, Bishops, Knights exceeds 8
            // (which can result only by promotion)
            if ((piece_count (c, PAWN) +
                std::max<int32_t> (piece_count<NIHT> (c) - 2, 0) +
                std::max<int32_t> (piece_count<BSHP> (c) - 2, 0) +
                std::max<int32_t> (piece_count<ROOK> (c) - 2, 0) +
                std::max<int32_t> (piece_count<QUEN> (c) - 1, 0)) > 8)
            {
                return false; // Too many Promoted Piece of color
            }

            if (piece_count (c, BSHP) > 1)
            {
                Bitboard bishops = colors & pieces (BSHP);

                uint8_t bishop_count[CLR_NO];
                bishop_count[WHITE] = pop_count<FULL> (LT_SQ_bb & bishops);
                bishop_count[BLACK] = pop_count<FULL> (DR_SQ_bb & bishops);

                if ((piece_count (c, PAWN) +
                    std::max<int32_t> (bishop_count[WHITE] - 1, 0) +
                    std::max<int32_t> (bishop_count[BLACK] - 1, 0)) > 8)
                {
                    return false; // Too many Promoted BISHOP of color
                }
            }

            // There should be one and only one KING of color
            Bitboard kings = colors & pieces (KING);
            if (!kings || more_than_one (kings)) return false;
        }

        // The intersection of the white and black pieces must be empty
        if (pieces (WHITE) & pieces (BLACK)) return false;

        Bitboard occ = pieces ();
        // The union of the white and black pieces must be equal to occupied squares
        if ((pieces (WHITE) | pieces (BLACK)) != occ) return false;
        if ((pieces (WHITE) ^ pieces (BLACK)) != occ) return false;

        // The intersection of separate piece type must be empty
        for (PType t1 = PAWN; t1 <= KING; ++t1)
        {
            for (PType t2 = PAWN; t2 <= KING; ++t2)
            {
                if (t1 != t2 && (pieces (t1) & pieces (t2))) return false;
            }
        }
        // The union of separate piece type must be equal to occupied squares
        if ((pieces (PAWN) | pieces (NIHT) | pieces (BSHP) | pieces (ROOK) | pieces (QUEN) | pieces (KING)) != occ) return false;
        if ((pieces (PAWN) ^ pieces (NIHT) ^ pieces (BSHP) ^ pieces (ROOK) ^ pieces (QUEN) ^ pieces (KING)) != occ) return false;

        // PAWN rank should not be 1/8
        if ((pieces (PAWN) & (R1_bb | R8_bb))) return false;
    }

    if (++(*step), debug_piece_list)
    {
        for (Color c = WHITE; c <= BLACK; ++c)
        {
            for (PType t = PAWN; t <= KING; ++t)
            {
                Piece p = (c | t);
                for (uint8_t cnt = 0; cnt < piece_count (c, t); ++cnt)
                {
                    if (_piece_arr[_piece_list[c][t][cnt]] != p) return false;
                }
            }
        }
    }

    *step = 0;
    return true;
}
