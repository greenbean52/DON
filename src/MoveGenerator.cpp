#include "MoveGenerator.h"
#include "Position.h"
#include "BitCount.h"
#include "BitScan.h"

namespace MoveGenerator {

    using namespace BitBoard;

#undef SERIALIZE
#undef SERIALIZE_PAWNS

    // Fill moves in the list for any piece using a very common while loop, no fancy.
#define SERIALIZE(lst_move, org, moves)         while (moves) { lst_move.emplace_back (mk_move<NORMAL> ((org), pop_lsb(moves))); }
    // Fill moves in the list for pawns, where the 'delta' is the distance b/w 'org' and 'dst' square.
#define SERIALIZE_PAWNS(lst_move, delta, moves) while (moves) { Square dst = pop_lsb (moves); lst_move.emplace_back (mk_move<NORMAL> (dst - (delta), dst)); }

    namespace {

#pragma region Move Generators

        template<GType G, PType T>
        // Move Generator for PIECE
        struct Generator
        {

        public:
            // Generates piece common move
            static inline void generate (MoveList &lst_move, const Position &pos, Color c, Bitboard target, const CheckInfo *ci = NULL)
                //template<GType G, PType T>
                //inline void Generator<G, T>::generate (MoveList &lst_move, const Position &pos, Color c, Bitboard target, const CheckInfo *ci)
            {
                //ASSERT ((KING != T) && (PAWN != T));
                static_assert ((KING != T) && (PAWN != T), "T must not be KING & PAWN");

                Bitboard occ = pos.pieces ();
                //Piece piece = (c | T);
                const SquareList pl = pos.list<T>(c); //pos[piece];

                std::for_each (pl.cbegin (), pl.cend (), [&] (Square org)
                {
                    if ((CHECK == G) || (QUIET_CHECK == G))
                    {
                        if (ci)
                        {
                            if ((BSHP == T) || (ROOK == T) || (QUEN == T) &&
                                !(attacks_bb<T> (org) & target & ci->checking_bb[T]))
                            {
                                return;
                            }
                            if (UNLIKELY (ci->check_discovers) && (ci->check_discovers & org))
                            {
                                return;
                            }
                        }
                    }

                    Bitboard moves = attacks_bb<T> (org, occ) & target;
                    if ((CHECK == G) || (QUIET_CHECK == G))
                    {
                        if (ci) moves &= ci->checking_bb[T];
                    }

                    SERIALIZE (lst_move, org, moves);
                });
            }

        };

        template<GType G>
        // Move Generator for KING
        struct Generator<G, KING>
        {

        public:
            // Generates KING common move
            static inline void generate (MoveList &lst_move, const Position &pos, Color clr, Bitboard target, const CheckInfo *ci = NULL)
                //template<GType G>
                //void Generator<G, KING>::generate (MoveList &lst_move, const Position &pos, Color clr, Bitboard target, const CheckInfo *ci)
            {
                //static_assert ((EVASION != G), "G must not be EVASION");
                if (EVASION != G)
                {
                    if ((CHECK != G) && (QUIET_CHECK != G))
                    {
                        Square k_sq    = pos.king_sq (clr);
                        Bitboard moves = attacks_bb<KING> (k_sq) & target;
                        SERIALIZE (lst_move, k_sq, moves);
                    }

                    if (CAPTURE != G)
                    {
                        if (!pos.castle_impeded (clr) && pos.can_castle (clr) && !pos.checkers ())
                        {
                            if (!pos.castle_impeded (clr, CS_K) && pos.can_castle (clr, CS_K))
                            {
                                generate_castling<CS_K> (lst_move, pos, clr, ci);
                            }
                            if (!pos.castle_impeded (clr, CS_Q) && pos.can_castle (clr, CS_Q))
                            {
                                generate_castling<CS_Q> (lst_move, pos, clr, ci);
                            }
                        }
                    }
                }
            }

            template<CSide SIDE>
            // Generates KING castling move
            static inline void generate_castling (MoveList &lst_move, const Position &pos, Color clr, const CheckInfo *ci /*= NULL*/)
                //template<GType G>
                //template<CSide SIDE>
                //void Generator<G, KING>::generate_castling (MoveList &lst_move, const Position &pos, Color clr, const CheckInfo *ci)
            {
                //static_assert ((EVASION != G) && (CHECK != G), "G must not be EVASION & CHECK");

                ASSERT (!pos.castle_impeded (clr, SIDE));
                ASSERT (pos.can_castle (clr, SIDE));
                ASSERT (!pos.checkers ());

                if (pos.castle_impeded (clr, SIDE) || !pos.can_castle (clr, SIDE) || pos.checkers ()) return;

                Square org_king = pos.king_sq (clr);
                Square org_rook = pos.castle_rook (clr, SIDE);
                Square dst_king = rel_sq (clr, (CS_Q == SIDE) ? SQ_WK_Q : SQ_WK_K);

                Bitboard enemies = pos.pieces (~clr);

                Delta step = (CS_Q == SIDE) ? DEL_W : DEL_E;
                for (Square s = org_king + step; s != dst_king; s += step)
                {
                    if (pos.attackers_to (s) & enemies)
                    {
                        return;
                    }
                }

                // Because we generate only legal castling moves we need to verify that
                // when moving the castling rook we do not discover some hidden checker.
                // For instance an enemy queen in SQ_A1 when castling rook is in SQ_B1.
                Move m = mk_move<CASTLE> (org_king, org_rook);

                switch (G)
                {
                case CHECK:
                case QUIET_CHECK:
                    if (ci)
                    {
                        if (pos.check (m, *ci))
                        {
                            lst_move.emplace_back (m);
                        }
                    }
                    break;

                default: lst_move.emplace_back (m);    break;
                }
            }

        };

        template<GType G>
        // Move Generator for PAWN
        struct Generator<G, PAWN>
        {

        public:
            template<Color C>
            // Generates PAWN common move
            static inline void generate (MoveList &lst_move, const Position &pos, Bitboard target, const CheckInfo *ci = NULL)
                //template<GType G>
                //template<Color C>
                //void Generator<G, PAWN>::generate<> (MoveList &lst_move, const Position &pos, Bitboard target, const CheckInfo *ci)
            {
                const Color C_   = ((WHITE == C) ? BLACK : WHITE);
                const Delta PUSH = ((WHITE == C) ? DEL_N  : DEL_S);
                const Delta RCAP = ((WHITE == C) ? DEL_NE : DEL_SW);
                const Delta LCAP = ((WHITE == C) ? DEL_NW : DEL_SE);

                Bitboard bbRR8 = rel_rank_bb (C, R_8);
                Bitboard bbRR7 = rel_rank_bb (C, R_7);
                Bitboard bbRR3 = rel_rank_bb (C, R_3);

                Bitboard pawns = pos.pieces (C, PAWN);
                Bitboard pawns_on_R7 = pawns &  bbRR7;
                Bitboard pawns_on_Rx = pawns & ~bbRR7;
                Bitboard occ = pos.pieces ();

                Bitboard enemies;
                switch (G)
                {
                case EVASION: enemies = pos.pieces (C_) & target; break;
                case CAPTURE: enemies = target;                   break;
                default:      enemies = pos.pieces (C_);          break;
                }

                Bitboard empty = U64 (0);
                // Pawn single-push and double-push, no promotions
                if (CAPTURE != G)
                {
                    empty = ((QUIET == G) || (QUIET_CHECK == G) ? target : ~occ);

                    Bitboard push_1 = shift_del<PUSH> (pawns_on_Rx) & empty;
                    Bitboard push_2 = shift_del<PUSH> (push_1 & bbRR3) & empty;

                    switch (G)
                    {
                    case EVASION:
                        // only blocking squares are important
                        push_1 &= target;
                        push_2 &= target;
                        break;

                    case CHECK:
                    case QUIET_CHECK:
                        if (ci)
                        {
                            Bitboard attack = attacks_bb<PAWN> (C_, ci->king_sq);

                            push_1 &= attack;
                            push_2 &= attack;

                            // pawns which give discovered check
                            Bitboard pawns_chk_dis = pawns_on_Rx & ci->check_discovers;
                            // Add pawn pushes which give discovered check. This is possible only
                            // if the pawn is not on the same file as the enemy king, because we
                            // don't generate captures. Note that a possible discovery check
                            // promotion has been already generated among captures.
                            if (pawns_chk_dis)
                            {
                                Bitboard push_cd_1 = shift_del<PUSH>(pawns_chk_dis) & empty;
                                Bitboard push_cd_2 = shift_del<PUSH>(push_cd_1 & bbRR3) & empty;

                                push_1 |= push_cd_1;
                                push_2 |= push_cd_2;
                            }
                        }
                        break;
                    }

                    SERIALIZE_PAWNS (lst_move, Delta (PUSH << 0), push_1);
                    SERIALIZE_PAWNS (lst_move, Delta (PUSH << 1), push_2);
                }
                // Pawn normal and en-passant captures, no promotions
                if ((QUIET != G) && (QUIET_CHECK != G))
                {
                    Bitboard attacksL = shift_del<LCAP> (pawns_on_Rx) & enemies;
                    Bitboard attacksR = shift_del<RCAP> (pawns_on_Rx) & enemies;;

                    SERIALIZE_PAWNS (lst_move, LCAP, attacksL);
                    SERIALIZE_PAWNS (lst_move, RCAP, attacksR);

                    Square ep_sq = pos.en_passant ();
                    if (SQ_NO != ep_sq)
                    {
                        ASSERT (_rank (ep_sq) == rel_rank (C, R_6));

                        Bitboard bbRR5 = rel_rank_bb (C, R_5);
                        Bitboard pawns_on_R5 = pawns_on_Rx & bbRR5;
                        if (pawns_on_R5)
                        {
                            // An en-passant capture can be an evasion only if the checking piece
                            // is the double pushed pawn and so is in the target. Otherwise this
                            // is a discovery check and we are forced to do otherwise.
                            // All time except when EVASION then 2nd condition must true
                            if ((EVASION != G) || (target & (ep_sq - PUSH)))
                            {
                                Bitboard pawns_ep = attacks_bb<PAWN>(C_, ep_sq) & pawns_on_R5;
                                ASSERT (pawns_ep);
                                ASSERT (pop_count<FULL> (pawns_ep) <= 2);
                                while (pawns_ep)
                                {
                                    lst_move.emplace_back (mk_move<ENPASSANT> (pop_lsb (pawns_ep), ep_sq));
                                }
                            }
                        }
                    }
                }
                // Promotions (queening and under-promotions)
                if (pawns_on_R7)
                {
                    // All time except when EVASION then 2nd condition must true
                    if ((EVASION != G) || (target & bbRR8))
                    {
                        switch (G)
                        {
                        case EVASION: empty &= target;        break;
                        case CAPTURE: empty = ~pos.pieces (); break;
                        }

                        //if (CAPTURE != G)
                        {
                            generate_promotion<PUSH> (lst_move, pawns_on_R7, empty, ci);
                        }

                        if ((QUIET != G) && (QUIET_CHECK != G))
                        {
                            generate_promotion<LCAP> (lst_move, pawns_on_R7, enemies, ci);
                            generate_promotion<RCAP> (lst_move, pawns_on_R7, enemies, ci);
                        }
                    }
                }
            }

            template<Delta D>
            // Generates PAWN promotion move
            static inline void generate_promotion (MoveList &lst_move, Bitboard pawns_on_R7, Bitboard target, const CheckInfo *ci /*= NULL*/)
                //template<GType G>
                //template<Delta D>
                //void Generator<G, PAWN>::generate_promotion (MoveList &lst_move, Bitboard pawns_on_R7, Bitboard target, const CheckInfo *ci)
            {
                static_assert ((DEL_NE == D || DEL_NW == D || DEL_SE == D || DEL_SW == D || DEL_N == D || DEL_S == D), "D may be wrong");

                //if (pawns_on_R7)
                {
                    Bitboard promotes = shift_del<D> (pawns_on_R7) & target;
                    while (promotes)
                    {
                        Square dst = pop_lsb (promotes);
                        Square org = dst - D;

                        if ( (RELAX == G) || (EVASION == G) ||
                            ((CAPTURE == G) && (DEL_NE == D || DEL_NW == D || DEL_SE == D || DEL_SW == D)))
                        {
                            lst_move.emplace_back (mk_move<PROMOTE> (org, dst, QUEN));
                        }

                        if ( (RELAX == G) || (EVASION == G) ||
                            ((QUIET == G) && (DEL_N == D || DEL_S == D)))
                        {
                            lst_move.emplace_back (mk_move<PROMOTE> (org, dst, ROOK));
                            lst_move.emplace_back (mk_move<PROMOTE> (org, dst, BSHP));
                            lst_move.emplace_back (mk_move<PROMOTE> (org, dst, NIHT));
                        }

                        // Knight-promotion is the only one that can give a direct check
                        // not already included in the queen-promotion (queening).
                        if ((CHECK == G) || (QUIET_CHECK == G))
                        {
                            if (ci && attacks_bb<NIHT> (dst) & ci->king_sq)
                            {
                                lst_move.emplace_back (mk_move<PROMOTE> (org, dst, NIHT));
                            }
                        }
                        else
                        {
                            (void) ci; // silence a warning under MSVC
                        }
                    }
                }
            }

        };

#pragma endregion

        template<Color C, GType G>
        // Generates all pseudo-legal moves of color for target.
        inline void generate_color (MoveList &lst_move, const Position &pos, Bitboard target, const CheckInfo *ci = NULL)
        {
            Generator<G, PAWN>::generate<C> (lst_move, pos, target, ci);
            Generator<G, NIHT>::generate (lst_move, pos, C, target, ci);
            Generator<G, BSHP>::generate (lst_move, pos, C, target, ci);
            Generator<G, ROOK>::generate (lst_move, pos, C, target, ci);
            Generator<G, QUEN>::generate (lst_move, pos, C, target, ci);

            if (EVASION != G)
            {
                Generator<G, KING>::generate (lst_move, pos, C, target, ci);
            }
        }

        inline void filter_illegal (MoveList &lst_move, const Position &pos)
        {
            Square k_sq      = pos.king_sq (pos.active ());
            Bitboard pinneds = pos.pinneds ();

            MoveList::iterator itr = lst_move.begin ();
            while (itr != lst_move.end ())
            {
                Move m = *itr;
                if (((sq_org (m) == k_sq) || pinneds || (ENPASSANT == _mtype (m))) &&
                    !pos.legal (m, pinneds))
                {
                    itr = lst_move.erase (itr);
                }
                else
                {
                    ++itr;
                }
            }
        }

    }

#pragma region Generates

    template<GType G>
    // Generates all pseudo-legal moves.
    MoveList generate (const Position &pos)
    {
        //ASSERT (RELAX == G || CAPTURE == G || QUIET == G);
        static_assert (RELAX == G || CAPTURE == G || QUIET == G, "G must be RELAX | CAPTURE | QUIET");
        ASSERT (!pos.checkers ());

        MoveList lst_move;

        Color active = pos.active ();
        Color pasive = ~active;

        Bitboard target = U64 (0);
        //CheckInfo *ci = NULL;
        switch (G)
        {
        case CAPTURE: target = pos.pieces (pasive);  break;

        case QUIET  : target = pos.empties ();       break;

        case RELAX  : target = ~pos.pieces (active); break;
        }

        switch (active)
        {
        case WHITE: generate_color<WHITE, G> (lst_move, pos, target); break;
        case BLACK: generate_color<BLACK, G> (lst_move, pos, target); break;
        }

        return lst_move;
    }

    // --------------------------------
    // explicit template instantiations
    template MoveList generate<RELAX>   (const Position &pos);
    template MoveList generate<CAPTURE> (const Position &pos);
    template MoveList generate<QUIET>   (const Position &pos);
    // --------------------------------

    template<>
    // Generates all pseudo-legal non-captures and knight underpromotions moves that give check.
    MoveList generate<QUIET_CHECK> (const Position &pos)
    {
        ASSERT (!pos.checkers ());

        MoveList lst_move;
        Color active = pos.active ();
        //Color pasive = ~active;
        Bitboard occ = pos.pieces ();
        Bitboard empty = ~occ;

        CheckInfo ci (pos);
        Bitboard discovers = ci.check_discovers & ~pos.pieces (active, PAWN);
        while (discovers)
        {
            Square org = pop_lsb (discovers);
            PType type = _ptype (pos[org]);

            Bitboard moves = U64 (0);
            switch (type)
            {
            case PAWN: continue; // Will be generated together with direct checks
            case NIHT: moves = attacks_bb<NIHT> (org) & empty;      break;
            case BSHP: moves = attacks_bb<BSHP> (org, occ) & empty; break;
            case ROOK: moves = attacks_bb<ROOK> (org, occ) & empty; break;
            case QUEN: moves = attacks_bb<QUEN> (org, occ) & empty; break;
            case KING: moves = attacks_bb<KING> (org) & empty &
                           ~attacks_bb<QUEN> (ci.king_sq);          break;
            }

            SERIALIZE (lst_move, org, moves);
        }

        switch (active)
        {
        case WHITE: generate_color<WHITE, QUIET_CHECK> (lst_move, pos, empty, &ci); break;
        case BLACK: generate_color<BLACK, QUIET_CHECK> (lst_move, pos, empty, &ci); break;
        }

        return lst_move;
    }

    template<>
    // Generates all pseudo-legal check giving moves.
    MoveList generate<CHECK>       (const Position &pos)
    {
        MoveList lst_move;

        Color active = pos.active ();
        Color pasive = ~active;
        Bitboard occ = pos.pieces ();
        Bitboard target = ~pos.pieces (active);

        CheckInfo ci (pos);
        Bitboard discovers = ci.check_discovers & ~pos.pieces (active, PAWN);
        while (discovers)
        {
            Square org = pop_lsb (discovers);
            PType type = _ptype (pos[org]);

            Bitboard moves = U64 (0);
            switch (type)
            {
            case PAWN: continue; // Will be generated together with direct checks
            case NIHT: moves = attacks_bb<NIHT> (org)      & target; break;
            case BSHP: moves = attacks_bb<BSHP> (org, occ) & target; break;
            case ROOK: moves = attacks_bb<ROOK> (org, occ) & target; break;
            case QUEN: moves = attacks_bb<QUEN> (org, occ) & target; break;
            case KING: moves = attacks_bb<KING> (org)      & target &
                           ~attacks_bb<QUEN> (ci.king_sq);           break;
            }

            SERIALIZE (lst_move, org, moves);
        }

        switch (active)
        {
        case WHITE: generate_color<WHITE, CHECK> (lst_move, pos, target, &ci); break;
        case BLACK: generate_color<BLACK, CHECK> (lst_move, pos, target, &ci); break;
        }

        return lst_move;
    }

    template<>
    // Generates all pseudo-legal check evasions moves when the side to move is in check.
    MoveList generate<EVASION>     (const Position &pos)
    {
        MoveList lst_move;

        Color active = pos.active ();
        Color pasive = ~active;
        Bitboard checkers = pos.checkers ();
        uint8_t num_checkers = pop_count<FULL> (checkers);
        ASSERT (num_checkers != 0); // If any checker exists

        Square k_sq      = pos.king_sq (active);
        Bitboard mocc    = pos.pieces () - k_sq;
        Bitboard friends = pos.pieces (active);
        Bitboard enemies = pos.pieces (pasive);

        // Generates evasions for king, capture and non-capture moves excluding friends
        Bitboard moves = attacks_bb<KING> (k_sq) & ~friends;

        // Remove squares attacked by enemies, from the king evasions.
        // so to skip known illegal moves avoiding useless legality check later.
        for (uint32_t k = 0; _deltas_type[KING][k]; ++k)
        {
            Square sq = k_sq + _deltas_type[KING][k];
            if (_ok (sq))
            {
                if ((moves & sq) && (pos.attackers_to (sq, mocc) & enemies))
                {
                    moves -= sq;
                }
            }
        }

        SERIALIZE (lst_move, k_sq, moves);
        if (1 == num_checkers && pop_count<FULL> (friends) > 1)
        {
            // Generates blocking evasions or captures of the checking piece
            Bitboard target = checkers | betwen_sq_bb (scan_lsb (checkers), k_sq);
            switch (active)
            {
            case WHITE: generate_color<WHITE, EVASION> (lst_move, pos, target); break;
            case BLACK: generate_color<BLACK, EVASION> (lst_move, pos, target); break;
            }
        }

        return lst_move;
    }

    template<>
    // Generates all legal moves.
    MoveList generate<LEGAL>       (const Position &pos)
    {
        MoveList lst_move = pos.checkers () ?
            generate<EVASION> (pos) :
            generate<RELAX> (pos);

        filter_illegal (lst_move, pos);

        return lst_move;
    }

#pragma endregion

#undef SERIALIZE
#undef SERIALIZE_PAWNS

}
