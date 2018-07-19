#include "Evaluator.h"

#include <ostream>
#include "BitBoard.h"
#include "Material.h"
#include "Notation.h"
#include "Option.h"
#include "Pawns.h"
#include "Thread.h"

using namespace std;
using namespace BitBoard;

namespace {

    enum Term : u08
    {
        // The first 6 entries are for PieceType
        MATERIAL = NONE,
        IMBALANCE,
        INITIATIVE,
        MOBILITY,
        THREAT,
        PASSER,
        SPACE,
        TOTAL,
    };

    Score Scores[TOTAL + 1][CLR_NO];

    void clear ()
    {
        std::memset (Scores, SCORE_ZERO, sizeof (Scores));
    }

    void write (Term term, Color c, Score score)
    {
        Scores[term][c] = score;
    }
    void write (Term term, Score wscore, Score bscore = SCORE_ZERO)
    {
        write (term, WHITE, wscore);
        write (term, BLACK, bscore);
    }

    ostream& operator<< (ostream &os, Term term)
    {
        auto *score = Scores[term];
        switch (term)
        {
        case Term::MATERIAL:
        case Term::IMBALANCE:
        case Term::INITIATIVE:
        case Term::TOTAL:
            os << " | ----- -----"
               << " | ----- -----";
            break;
        default:
            os << " | " << score[WHITE]
               << " | " << score[BLACK];
            break;
        }
        os << " | " << score[WHITE] - score[BLACK] << std::endl;
        return os;
    }


#define S(mg, eg) mk_score (mg, eg)

    constexpr Score Mobility[4][28] =
    {
        { // Knight
            S(-75,-76), S(-57,-54), S( -9,-28), S( -2,-10), S(  6,  5), S( 14, 12),
            S( 22, 26), S( 29, 29), S( 36, 29)
        },
        { // Bishop
            S(-48,-59), S(-20,-23), S( 16, -3), S( 26, 13), S( 38, 24), S( 51, 42),
            S( 55, 54), S( 63, 57), S( 63, 65), S( 68, 73), S( 81, 78), S( 81, 86),
            S( 91, 88), S( 98, 97)
        },
        { // Rook
            S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82),
            S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),
            S( 46,166), S( 48,169), S( 58,171)
        },
        { // Queen
            S(-39,-36), S(-21,-15), S(  3,  8), S(  3, 18), S( 14, 34), S( 22, 54),
            S( 28, 61), S( 41, 73), S( 43, 79), S( 48, 92), S( 56, 94), S( 60,104),
            S( 60,113), S( 66,120), S( 67,123), S( 70,126), S( 71,133), S( 73,136),
            S( 79,140), S( 88,143), S( 88,148), S( 99,166), S(102,170), S(102,175),
            S(106,184), S(109,191), S(113,206), S(116,212)
        }
    };

    constexpr Score Outpost[2][2] =
    {
        { S(22, 6), S(36,12) },
        { S( 9, 2), S(15, 5) }
    };

    constexpr Score RookOnFile[2] =
    {
        S(20, 7), S(45,20)
    };

    constexpr Score MinorThreat[NONE] =
    {
        S( 0,31), S(39,42), S(57,44), S(68,112), S(47,120), S( 0, 0)
    };
    constexpr Score MajorThreat[NONE] =
    {
        S( 0,24), S(38,71), S(38,61), S( 0, 38), S(36, 38), S( 0, 0)
    };

    constexpr Score PasserFile[F_NO/2] =
    {
        S( -1,  7), S(  0,  9), S( -9, -8), S(-30,-14)
    };
    constexpr Score PasserRank[R_NO] =
    {
        S( 0, 0), S(  5, 18), S( 12, 23), S( 10, 31), S( 57, 62), S(163,167), S(271,250), S( 0, 0)
    };

    constexpr Score MinorBehindPawn =   S( 16,  0);
    constexpr Score MinorKingProtect =  S(  6,  6);
    constexpr Score BishopOnDiagonal =  S( 22,  0);
    constexpr Score BishopPawns =       S(  3,  7);
    constexpr Score BishopTrapped =     S( 50, 50);
    constexpr Score RookOnPawns =       S(  8, 24);
    constexpr Score RookTrapped =       S( 92,  0);
    constexpr Score QueenWeaken =       S( 50, 10);
    constexpr Score PawnLessFlank =     S( 20, 80);
    constexpr Score KingUnderAttack =   S(  8,  0);
    constexpr Score PawnWeakUnopposed = S(  5, 29);
    constexpr Score PieceHanged =       S( 52, 30);
    constexpr Score SafePawnThreat =    S(173,102);
    constexpr Score PawnPushThreat =    S( 45, 40);
    constexpr Score RankThreat =        S( 16,  3);
    constexpr Score KingThreat =        S( 23, 76);
    constexpr Score KnightQueenThreat = S( 21, 11);
    constexpr Score SliderQueenThreat = S( 42, 21);
    constexpr Score Connectivity =      S(  3,  1);
    constexpr Score Overloaded =        S( 10,  5);
    constexpr Score PasserHinder =      S(  4,  0);

#undef S

    constexpr i32 KingSafeCheck[NONE] =
    {
        0, 790, 435, 880, 780, 0
    };

    constexpr i32 KingAttack[NONE] =
    {
        0, 77, 55, 44, 10, 0
    };

    constexpr i32 PasserDanger[R_NO] =
    {
        0, 0, 0, 3, 7, 11, 20, 0
    };

    constexpr Value LazyThreshold = Value(1500);
    constexpr Value SpaceThreshold = Value(12222);

    // Evaluator class contains various evaluation functions.
    template<bool Trace>
    class Evaluator
    {
    private:

        const Position &pos;

        Pawns::Entry *pe = nullptr;
        Material::Entry *me = nullptr;

        Bitboard mob_area[CLR_NO];
        Score    mobility[CLR_NO];

        // Contains all squares attacked by the color and piece type.
        Bitboard ful_attacks[CLR_NO];
        // Contains all squares attacked by the color and piece type with pinned removed.
        Bitboard sgl_attacks[CLR_NO][PT_NO];
        // Contains all squares attacked by more than one pieces of a color, possibly via x-ray or by one pawn and one piece.
        Bitboard dbl_attacks[CLR_NO];

        Bitboard queen_attacks[CLR_NO][3];

        // Zone around the king which is considered by the king safety evaluation.
        // This consists of the squares directly adjacent to the king, and the three (or two, for a king on an edge file) squares two ranks in front of the king.
        // For instance, if black king is on g8, king_ring[BLACK] is a bitboard containing the squares f8, h8, f7, g7, h7, f6, g6 and h6.
        Bitboard king_ring[CLR_NO];
        // Number of pieces of the color, which attack a square in the king_ring of the enemy king.
        u08 king_attackers_count[CLR_NO];
        // Sum of the "weight" of the pieces of the color which attack a square in the king_ring of the enemy king.
        // The weights of the individual piece types are given by the KingAttack[piece-type]
        i32 king_attackers_weight[CLR_NO];
        // Number of attacks by the color to squares directly adjacent to the enemy king.
        // Pieces which attack more than one square are counted multiple times.
        u08 king_attacks_count[CLR_NO];

        template<Color> void initialize ();
        template<Color, PieceType> Score pieces ();
        template<Color> Score king ();
        template<Color> Score threats ();
        template<Color> Score passers ();
        template<Color> Score space ();

        Score initiative (Value) const;
        Scale scale (Value) const;

    public:
        Evaluator () = delete;
        Evaluator (const Evaluator&) = delete;
        Evaluator& operator= (const Evaluator&) = delete;

        Evaluator (const Position &p)
            : pos (p)
        {}

        Value value ();
    };

    /// initialize() computes king and pawn attacks, and the king ring bitboard of the color.
    template<bool Trace> template<Color Own>
    void Evaluator<Trace>::initialize ()
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;
        constexpr auto Push = WHITE == Own ? DEL_N : DEL_S;
        constexpr auto Pull = WHITE == Own ? DEL_S : DEL_N;

        std::fill_n (sgl_attacks[Own], i32(KING), 0);
        std::fill_n (queen_attacks[Own], 3, 0);

        Bitboard pinned_pawns = pos.si->king_blockers[Own] & pos.pieces (Own, PAWN);
        if (0 != pinned_pawns)
        {
            Bitboard loosed_pawns = pos.pieces (Own, PAWN) ^ pinned_pawns;
            sgl_attacks[Own][PAWN] = pawn_attacks_bb<Own> (loosed_pawns)
                                   | (  pawn_attacks_bb<Own> (pinned_pawns)
                                      & PieceAttacks[BSHP][pos.square<KING> (Own)]);
        }
        else
        {
            sgl_attacks[Own][PAWN] = pe->any_attacks[Own];
        }

        sgl_attacks[Own][KING] = PieceAttacks[KING][pos.square<KING> (Own)];

        ful_attacks[Own] = sgl_attacks[Own][KING]
                         | pe->any_attacks[Own];
        sgl_attacks[Own][NONE] = sgl_attacks[Own][KING]
                               | sgl_attacks[Own][PAWN];
        dbl_attacks[Own] = (  sgl_attacks[Own][KING]
                            | pe->dbl_attacks[Own])
                         & sgl_attacks[Own][PAWN];

        // Do not include in mobility area
        // - squares protected by enemy pawns
        // - square occupied by friend Queen and King
        // - squares occupied by block pawns (pawns blocked or on ranks 2-3)
        mob_area[Opp] = ~(  sgl_attacks[Own][PAWN]
                          | pos.pieces (Opp, QUEN, KING)
                          | (  pos.pieces (Opp, PAWN)
                             & (  LowRanks_bb[Opp]
                                | shift<Push> (pos.pieces ()))));
        mobility[Opp] = SCORE_ZERO;

        king_attackers_weight[Own] = 0;
        king_attacks_count[Own] = 0;
        if (pos.si->non_pawn_material (Own) >= VALUE_MG_ROOK + VALUE_MG_NIHT)
        {
            king_ring[Opp] = PieceAttacks[KING][pos.square<KING> (Opp)];
            if (R_1 == rel_rank (Opp, pos.square<KING> (Opp)))
            {
                king_ring[Opp] |= shift<Pull> (king_ring[Opp]);
            }
            if (F_H == _file (pos.square<KING> (Opp)))
            {
                king_ring[Opp] |= shift<DEL_W> (king_ring[Opp]);
            }
            else
            if (F_A == _file (pos.square<KING> (Opp)))
            {
                king_ring[Opp] |= shift<DEL_E> (king_ring[Opp]);
            }

            king_attackers_count[Own] = pop_count (king_ring[Opp] & sgl_attacks[Own][PAWN]);
        }
        else
        {
            king_ring[Opp] = 0;
            king_attackers_count[Own] = 0;
        }
    }

    /// pieces() evaluates the pieces of the color and type.
    template<bool Trace> template<Color Own, PieceType PT>
    Score Evaluator<Trace>::pieces ()
    {
        static_assert (NIHT == PT
                    || BSHP == PT
                    || ROOK == PT
                    || QUEN == PT, "PT incorrect");

        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;
        constexpr auto Push = WHITE == Own ? DEL_N : DEL_S;
        constexpr auto Pull = WHITE == Own ? DEL_S : DEL_N;

        auto score = SCORE_ZERO;

        for (auto s : pos.squares[Own][PT])
        {
            assert(pos[s] == (Own|PT));
            Bitboard attacks;
            // Find attacked squares, including x-ray attacks for bishops and rooks
            attacks = NIHT == PT ? PieceAttacks[NIHT][s] :
                      BSHP == PT ? attacks_bb<BSHP> (s, pos.pieces () ^ ((pos.pieces (Own, QUEN, BSHP) & ~pos.si->king_blockers[Own]) | pos.pieces (Opp, QUEN))) :
                      ROOK == PT ? attacks_bb<ROOK> (s, pos.pieces () ^ ((pos.pieces (Own, QUEN, ROOK) & ~pos.si->king_blockers[Own]) | pos.pieces (Opp, QUEN))) :
                      QUEN == PT ? attacks_bb<QUEN> (s, pos.pieces () ^ ((pos.pieces (Own, QUEN)       & ~pos.si->king_blockers[Own]))) : (assert(false), 0);

            ful_attacks[Own] |= attacks;

            if (QUEN == PT)
            {
                queen_attacks[Own][0] |= PieceAttacks[NIHT][s];
                queen_attacks[Own][1] |= PieceAttacks[BSHP][s] & attacks;
                queen_attacks[Own][2] |= PieceAttacks[ROOK][s] & attacks;
            }

            if (contains (pos.si->king_blockers[Own], s))
            {
                attacks &= strline_bb (pos.square<KING> (Own), s);
            }

            if (BSHP == PT)
            {
                Bitboard att = attacks & pos.pieces (Own) & ~pos.si->king_blockers[Own];
                Bitboard bp = att & front_rank_bb (Own, s) & pos.pieces (PAWN);
                dbl_attacks[Own] |= sgl_attacks[Own][NONE]
                                  & (  attacks
                                     | (0 != bp ? pawn_attacks_bb<Own> (bp) & PieceAttacks[BSHP][s] : 0));
            }
            else
            if (QUEN == PT)
            {
                Bitboard att = attacks & pos.pieces (Own) & ~pos.si->king_blockers[Own];
                Bitboard qp = att & front_rank_bb (Own, s) & pos.pieces (PAWN);
                Bitboard qb = att & PieceAttacks[BSHP][s]  & pos.pieces (BSHP);
                Bitboard qr = att & PieceAttacks[ROOK][s]  & pos.pieces (ROOK);
                dbl_attacks[Own] |= sgl_attacks[Own][NONE]
                                  & (  attacks
                                     | (0 != qp ? pawn_attacks_bb<Own> (qp) & PieceAttacks[BSHP][s] : 0)
                                     | (0 != qb ? attacks_bb<BSHP> (s, pos.pieces () ^ qb) : 0)
                                     | (0 != qr ? attacks_bb<ROOK> (s, pos.pieces () ^ qr) : 0));
            }
            else
            {
                dbl_attacks[Own] |= sgl_attacks[Own][NONE]
                                  & attacks;
            }

            sgl_attacks[Own][PT]   |= attacks;
            sgl_attacks[Own][NONE] |= attacks;

            if (0 != (king_ring[Opp] & attacks))
            {
                ++king_attackers_count[Own];
                king_attackers_weight[Own] += KingAttack[PT];
                king_attacks_count[Own] += pop_count (sgl_attacks[Opp][KING] & attacks);
            }

            auto mob = pop_count (mob_area[Own] & attacks);
            assert(0 <= mob && mob <= 27);

            // Bonus for piece mobility
            mobility[Own] += Mobility[PT - 1][mob];

            Bitboard b;
            // Special extra evaluation for pieces
            if (   NIHT == PT
                || BSHP == PT)
            {
                // Bonus for minor behind a pawn
                if (contains (shift<Pull> (pos.pieces (PAWN)), s))
                {
                    score += MinorBehindPawn;
                }

                // Penalty for distance from the friend king
                score -= MinorKingProtect * dist (s, pos.square<KING> (Own));

                b = Outposts_bb[Own]
                  & ~pe->attack_span[Opp];
                // Bonus for knight outpost squares
                if (contains (b, s))
                {
                    score += Outpost[PT - 1][contains (sgl_attacks[Own][PAWN], s) ? 1 : 0] * 2;
                }
                else
                {
                    b &= attacks
                      & ~pos.pieces (Own);
                    if (0 != b)
                    {
                        score += Outpost[PT - 1][0 != (sgl_attacks[Own][PAWN] & b) ? 1 : 0] * 1;
                    }
                }

                if (BSHP == PT)
                {
                    // Penalty for pawns on the same color square as the bishop,
                    // more when the center files are blocked with pawns.
                    b = pos.pieces (Own, PAWN)
                      & Side_bb[CS_NO]
                      & shift<Pull> (pos.pieces ());
                    score -= BishopPawns
                           * (1 + pop_count (b))
                           * pe->color_count[Own][color (s)];

                    // Bonus for bishop on a long diagonal which can "see" both center squares
                    if (   contains (Diagonals_bb, s)
                        && more_than_one (Center_bb & (attacks_bb<BSHP> (s, pos.pieces (PAWN)) | s)))
                    {
                        score += BishopOnDiagonal;
                    }

                    if (bool(Options["UCI_Chess960"]))
                    {
                        // An important Chess960 pattern: A cornered bishop blocked by a friend pawn diagonally in front of it.
                        // It is a very serious problem, especially when that pawn is also blocked.
                        // Bishop (white or black) on a1/h1 or a8/h8 which is trapped by own pawn on b2/g2 or b7/g7.
                        if (   1 >= mob
                            && contains (FA_bb|FH_bb, s)
                            && R_1 == rel_rank (Own, s))
                        {
                            auto del = Delta((F_E - _file (s))/3) + Push;
                            if (contains (pos.pieces (Own, PAWN), s+del))
                            {
                                score -= BishopTrapped
                                       * (!contains (pos.pieces (), s + del + Push) ?
                                              !contains (pos.pieces (Own, PAWN), s + del + del) ?
                                                  1 : 2 : 4);
                            }
                        }
                    }
                }
            }
            else
            if (ROOK == PT)
            {
                // Bonus for rook aligning with enemy pawns on the same rank/file
                if (   R_4 < rel_rank (Own, s)
                    && 0 != (b = pos.pieces (Opp, PAWN) & PieceAttacks[ROOK][s]))
                {
                    score += RookOnPawns * pop_count (b);
                }

                // Bonus for rook when on an open or semi-open file
                if (pe->file_semiopen<Own> (_file (s)))
                {
                    score += RookOnFile[pe->file_semiopen<Opp> (_file (s)) ? 1 : 0];
                }
                else
                // Penalty for rook when trapped by the king, even more if the king can't castle
                if (   3 >= mob
                    && R_5 > rel_rank (Own, s))
                {
                    auto kf = _file (pos.square<KING> (Own));
                    if ((kf < F_E) == (_file (s) < kf))
                    {
                        score -= (RookTrapped - mk_score (22 * mob, 0))
                               * (pos.si->can_castle (Own) ? 1 : 2);
                    }
                }
            }
            else
            if (QUEN == PT)
            {
                // Penalty for pin or discover attack on the queen
                b = 0;
                if (0 != (  pos.slider_blockers (s, Own, pos.pieces (QUEN), b, b)
                          & ~(  (  pos.pieces (Opp, PAWN)
                                 & file_bb (s)
                                 & ~pawn_attacks_bb<Own> (pos.pieces (Own)))
                              | (  pos.si->king_blockers[Opp]
                                 & pos.pieces (Opp)))))
                {
                    score -= QueenWeaken;
                }
            }
        }

        if (Trace)
        {
            write (Term(PT), Own, score);
        }

        return score;
    }

    /// king() evaluates the king of the color.
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::king ()
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        auto fk_sq = pos.square<KING> (Own);

        // King Safety: friend pawns shelter and enemy pawns storm
        u08 index = pe->king_safety_on<Own> (pos, fk_sq);
        Value safety = pe->king_safety[Own][index];
        if (   R_1 == rel_rank (Own, fk_sq)
            && pos.si->can_castle (Own))
        {
            if (   pos.si->can_castle (Own, CS_KING)
                && pos.expeded_castle (Own, CS_KING)
                && 0 == (pos.king_path_bb[Own][CS_KING] & ful_attacks[Opp])
                && safety < pe->king_safety[Own][0])
            {
                safety = pe->king_safety[Own][0];
            }
            if (   pos.si->can_castle (Own, CS_QUEN)
                && pos.expeded_castle (Own, CS_QUEN)
                && 0 == (pos.king_path_bb[Own][CS_QUEN] & ful_attacks[Opp])
                && safety < pe->king_safety[Own][1])
            {
                safety = pe->king_safety[Own][1];
            }
        }

        auto score = mk_score (safety, -16 * pe->king_pawn_dist[Own][index]);

        Bitboard b;
        // Main king safety evaluation
        if (king_attackers_count[Opp] + pos.count (Opp, QUEN) > 1)
        {
            i32 king_danger = 0;
            Bitboard unsafe_check = 0;

            // Attacked squares defended at most once by friend queen or king
            Bitboard weak_area =  sgl_attacks[Opp][NONE]
                               & ~dbl_attacks[Own]
                               & (   sgl_attacks[Own][KING]
                                  |  sgl_attacks[Own][QUEN]
                                  | ~sgl_attacks[Own][NONE]);

            // Safe squares where enemy's safe checks are possible on next move
            Bitboard safe_area = ~pos.pieces (Opp)
                               & (  ~sgl_attacks[Own][NONE]
                                  | (  weak_area
                                     & dbl_attacks[Opp]));

            Bitboard rook_attack = attacks_bb<ROOK> (fk_sq, pos.pieces () ^ pos.pieces (Own, QUEN));
            Bitboard bshp_attack = attacks_bb<BSHP> (fk_sq, pos.pieces () ^ pos.pieces (Own, QUEN));

            // Enemy queens safe checks
            b = (  rook_attack
                 | bshp_attack)
              &  sgl_attacks[Opp][QUEN]
              & ~sgl_attacks[Own][QUEN];
            if (0 != (b & safe_area))
            {
                king_danger += KingSafeCheck[QUEN];
            }

            b = rook_attack
              & sgl_attacks[Opp][ROOK];
            if (0 != (b & safe_area))
            {
                king_danger += KingSafeCheck[ROOK];
            }
            else
            {
                unsafe_check |= b;
            }

            b = bshp_attack
              & sgl_attacks[Opp][BSHP];
            if (0 != (b & safe_area))
            {
                king_danger += KingSafeCheck[BSHP];
            }
            else
            {
                unsafe_check |= b;
            }

            b = PieceAttacks[NIHT][fk_sq]
              & sgl_attacks[Opp][NIHT];
            if (0 != (b & safe_area))
            {
                king_danger += KingSafeCheck[NIHT];
            }
            else
            {
                unsafe_check |= b;
            }

            // Initialize the king danger, which will be transformed later into a score.
            // - number and types of the enemy's attacking pieces,
            // - number of attacked and undefended squares around friend king,
            // - quality of the pawn shelter ('mg score' safety).
            king_danger +=  1 * king_attackers_count[Opp]*king_attackers_weight[Opp]
                        +  69 * king_attacks_count[Opp]
                        + 185 * pop_count (king_ring[Own] & weak_area)
                        + 129 * pop_count (pos.si->king_blockers[Own] | (unsafe_check & mob_area[Opp]))
                        - 873 * (0 == pos.count (Opp, QUEN) ? 1 : 0)
                        -   6 * safety / 8
                        -   2;

            if (king_danger > 0)
            {
                // Transform the king danger into a score, and subtract it from the score
                king_danger = std::max (king_danger + mg_value (mobility[Opp] - mobility[Own]), 0);
                score -= mk_score (king_danger*king_danger / 0x1000, king_danger / 0x10);
            }
        }

        Bitboard kf_bb = KingFlank_bb[_file (fk_sq)];

        // Penalty for king on a pawn less flank
        if (0 == (  kf_bb
                  & pos.pieces (PAWN)))
        {
            score -= PawnLessFlank;
        }

        Bitboard e;

        // Squares attacked by enemy in friend king flank
        b = Camp_bb[Own]
          & kf_bb
          & sgl_attacks[Opp][NONE];
        // Squares attacked by enemy twice in friend king flank but not defended by friend pawns.
        e = b
          & dbl_attacks[Opp]
          & ~sgl_attacks[Own][PAWN];
        // King tropism, to anticipate slow motion attacks on friend king zone
        score -= KingUnderAttack * (pop_count (b) + pop_count (e));

        if (Trace)
        {
            write (Term(KING), Own, score);
        }

        return score;
    }

    /// threats() evaluates the threats of the color.
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::threats ()
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;
        constexpr auto Push = WHITE == Own ? DEL_N : DEL_S;
        constexpr Bitboard R3BB = WHITE == Own ? R3_bb : R6_bb;

        auto score = SCORE_ZERO;

        // Enemy non-pawns
        Bitboard nonpawns_enemies = pos.pieces (Opp)
                                  ^ pos.pieces (Opp, PAWN);
        // Squares defended by the opponent,
        // - attack the square with a pawn
        // - attack the square twice and not defended twice.
        Bitboard defended_area = sgl_attacks[Opp][PAWN]
                               | (   dbl_attacks[Opp]
                                  & ~dbl_attacks[Own]);
        // Enemy not defended and under attacked by any friend piece
        Bitboard attacked_weak_enemies =  pos.pieces (Opp)
                                       & ~defended_area
                                       &  sgl_attacks[Own][NONE];
        // Non-pawn enemies, defended by enemies
        Bitboard defended_nonpawns_enemies = nonpawns_enemies
                                           & defended_area;

        Bitboard b;

        if (0 != (defended_nonpawns_enemies | attacked_weak_enemies))
        {
            // Bonus according to the type of attacking pieces

            // Enemies attacked by minors
            b = (  attacked_weak_enemies
                   // Enemy defended non-pawns
                 | defended_nonpawns_enemies
                   // Enemy Rooks or Queens
                 | pos.pieces (Opp, ROOK, QUEN))
              & (  sgl_attacks[Own][NIHT]
                 | sgl_attacks[Own][BSHP]);
            while (0 != b)
            {
                auto s = pop_lsq (b);
                auto pt = ptype (pos[s]);
                score += MinorThreat[pt];
                if (PAWN != pt)
                {
                    score += RankThreat * rel_rank (Opp, s);
                }
            }
            if (0 != attacked_weak_enemies)
            {
                // Enemies attacked by majors
                b = (  attacked_weak_enemies
                       // Enemy Queens
                     | pos.pieces (Opp, QUEN))
                  & sgl_attacks[Own][ROOK];
                while (0 != b)
                {
                    auto s = pop_lsq (b);
                    auto pt = ptype (pos[s]);
                    score += MajorThreat[pt];
                    if (PAWN != pt)
                    {
                        score += RankThreat * rel_rank (Opp, s);
                    }
                }
                // Enemies attacked by king
                b = attacked_weak_enemies
                  & sgl_attacks[Own][KING];
                if (0 != b)
                {
                    score += KingThreat;
                }

                // Enemies attacked are hanging
                b = attacked_weak_enemies
                  & ~sgl_attacks[Opp][NONE];
                score += PieceHanged * pop_count (b);
            }

            // Bonus for overloaded: non-pawn enemies attacked once or more and defended exactly once
            b = nonpawns_enemies
              & sgl_attacks[Own][NONE]
              & sgl_attacks[Opp][NONE]
              & ~dbl_attacks[Opp];
            score += Overloaded * pop_count (b);
        }

        // Bonus for opponent unopposed weak pawns
        if (0 != pos.pieces (Own, ROOK, QUEN))
        {
            score += PawnWeakUnopposed * pop_count (pe->weak_unopposed[Opp]);
        }

        Bitboard safe_area =  sgl_attacks[Own][NONE]
                           | ~sgl_attacks[Opp][NONE];
        // Safe friend pawns
        b = safe_area
          & pos.pieces (Own, PAWN);
        b = nonpawns_enemies
          & pawn_attacks_bb<Own> (b)
          & sgl_attacks[Own][PAWN];
        score += SafePawnThreat * pop_count (b);

        // Friend pawns who can push on the next move
        b =  pos.pieces (Own, PAWN)
          & ~pos.si->king_blockers[Own];
        // Friend pawns push
        b =  shift<Push> (b)
          & ~pos.pieces ();
        b |= shift<Push> (b & R3BB)
          & ~pos.pieces ();
        // Friend pawns push safe
        b &= safe_area
          & ~sgl_attacks[Opp][PAWN];
        // Friend pawns push safe attacks an enemy piece not already attacked by pawn
        b =  pawn_attacks_bb<Own> (b)
          &  pos.pieces (Opp)
          & ~sgl_attacks[Own][PAWN];
        // Bonus for friend pawns push safely can attack an enemy piece not already attacked by pawn
        score += PawnPushThreat * pop_count (b);

        // Bonus for threats on the next moves against enemy queens
        if (0 != pos.pieces (Opp, QUEN))
        {
            Bitboard safe_threat = mob_area[Own]
                                 & ~defended_area;
            b = safe_threat
              & (sgl_attacks[Own][NIHT] & queen_attacks[Opp][0]);
            score += KnightQueenThreat * pop_count (b);

            b = safe_threat
              & (  (sgl_attacks[Own][BSHP] & queen_attacks[Opp][1])
                 | (sgl_attacks[Own][ROOK] & queen_attacks[Opp][2]))
              & dbl_attacks[Own];
            // Bonus for safe slider attack threats on enemy queen
            score += SliderQueenThreat * pop_count (b);
        }
        // Bonus for Connectivity: ensure that knights, bishops, rooks, and queens are protected
        b = pos.pieces (Own, NIHT, BSHP, ROOK, QUEN)
          & sgl_attacks[Own][NONE];
        score += Connectivity * pop_count (b);

        if (Trace)
        {
            write (Term::THREAT, Own, score);
        }

        return score;
    }

    /// passers() evaluates the passed pawns of the color.
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::passers ()
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;
        constexpr auto Push = WHITE == Own ? DEL_N : DEL_S;

        auto king_proximity = [&](Color c, Square s)
                              {
                                  return std::min (dist (pos.square<KING> (c), s), 5);
                              };

        auto score = SCORE_ZERO;

        Bitboard psr = pe->passers[Own];
        while (0 != psr)
        {
            auto s = pop_lsq (psr);
            assert(0 == (pos.pieces (Opp, PAWN) & shift<Push> (front_line_bb (Own, s))));

            i32 r = rel_rank (Own, s);
            i32 w = PasserDanger[r];

            // Base bonus depending on rank.
            Score bonus = PasserRank[r];

            auto push_sq = s + Push;

            if (0 != w)
            {
                // Adjust bonus based on the king's proximity
                if (!contains (pawn_pass_span (Own, s), pos.square<KING> (Opp)))
                {
                    bonus += mk_score(0, 5*w*king_proximity (Opp, push_sq));
                }
                bonus -= mk_score (0, 2*w*king_proximity (Own, push_sq));

                // If block square is not the queening square then consider also a second push.
                if (R_7 != r)
                {
                    bonus -= mk_score (0, 1*w*king_proximity (Own, push_sq + Push));
                }

                // If the pawn is free to advance.
                if (pos.empty (push_sq))
                {
                    // Squares to queen
                    Bitboard front_line = front_line_bb (Own, s);
                    Bitboard safe_front_line = front_line
                        ,  unsafe_front_line = front_line;
                    // If there is a rook or queen attacking/defending the pawn from behind, consider front squares.
                    // Otherwise consider only the squares in the pawn's path attacked or occupied by the enemy.
                    Bitboard behind_major = front_line_bb (Opp, s) & pos.pieces (ROOK, QUEN);
                    if (0 != behind_major)
                    {
                        behind_major &= attacks_bb<ROOK> (s, pos.pieces ());
                        assert(1 >= pop_count (behind_major));
                    }
                    Bitboard b;
                    // If there is no enemy rook or queen attacking the pawn from behind,
                    // consider only the squares in the pawn's path attacked or occupied by the enemy,
                    // Otherwise add all X-ray attacks by the enemy rook or queen.
                    if (   0 == (b = (behind_major & pos.pieces (Opp)))
                        || 0 != (b & pos.si->king_blockers[Opp]))
                    {
                        unsafe_front_line &= sgl_attacks[Opp][NONE] | pos.pieces (Opp);
                    }
                    // If there is no friend rook or queen attacking the pawn from behind,
                    // consider only the squares in the pawn's path attacked by the friend.
                    // Otherwise add all X-ray attacks by the friend rook or queen.
                    if (   0 == (b = (behind_major & pos.pieces (Own)))
                        || 0 != (b & pos.si->king_blockers[Own]))
                    {
                        safe_front_line &= sgl_attacks[Own][NONE];
                    }

                    i32 k;
                    // Give a big bonus if the path to the queen is not attacked,
                    // a smaller bonus if the block square is not attacked.
                    k = 0 != unsafe_front_line ?
                             contains (unsafe_front_line, push_sq) ?
                                0 : 9 : 20;
                    // Give a big bonus if the path to the queen is fully defended,
                    // a smaller bonus if the block square is defended.
                    k += safe_front_line != front_line ?
                            !contains (safe_front_line, push_sq) ?
                                0 : 4 : 6;

                    bonus += mk_score (k*w, k*w);
                }
                else
                // If the pawn is blocked by own pieces.
                if (contains (pos.pieces (Own), push_sq))
                {
                    bonus += mk_score (1*w + 2*r, 1*w + 2*r);
                }
            }

            // Scale down bonus for candidate passers which need more than one 
            // pawn push to become passed or have a pawn in front of them.
            if (   !pos.pawn_passed_at (Own, push_sq)
                || 0 != (pos.pieces (PAWN) & front_line_bb (Own, s)))
            {
                bonus /= 2;
            }

            score += bonus
                   + PasserFile[std::min (_file (s), ~_file (s))]
                   - PasserHinder * pop_count (front_line_bb (Own, s) & pos.pieces (Opp));
        }

        if (Trace)
        {
            write (Term::PASSER, Own, score);
        }

        return score;
    }

    /// space() evaluates the space of the color.
    /// The space evaluation is a simple bonus based on the number of safe squares
    /// available for minor pieces on the central four files on ranks 2-4
    /// Safe squares one, two or three squares behind a friend pawn are counted twice
    /// The aim is to improve play on opening
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::space ()
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;
        constexpr auto Pull = WHITE == Own ? DEL_S : DEL_N;
        constexpr auto Dull = WHITE == Own ? DEL_SS : DEL_NN;

        // Safe squares for friend pieces inside the area defined by SpaceMask.
        // - if not occupied by friend pawns
        // - if not attacked by an enemy pawns
        // - if defended by friend pieces or not attacked by enemy pieces.
        Bitboard safe_space = Space_bb[Own]
                            & Side_bb[CS_NO]
                            & ~pos.pieces (Own, PAWN)
                            & ~sgl_attacks[Opp][PAWN];

        // Find all squares which are at most three squares behind some friend pawn
        Bitboard behind = pos.pieces (Own, PAWN);
        behind |= shift<Pull> (behind);
        behind |= shift<Dull> (behind);
        i32 bonus = pop_count (safe_space) + pop_count (behind & safe_space);
        i32 weight = pos.count (Own) - 2 * pe->open_count;
        auto score = mk_score (bonus * weight * weight / 16, 0);

        if (Trace)
        {
            write (Term::SPACE, Own, score);
        }

        return score;
    }

    /// initiative() evaluates the initiative correction value for the position
    /// i.e. second order bonus/malus based on the known attacking/defending status of the players
    template<bool Trace>
    Score Evaluator<Trace>::initiative (Value eg) const
    {
        i32 outflanking = dist<File> (pos.square<KING> (WHITE), pos.square<KING> (BLACK))
                        - dist<Rank> (pos.square<KING> (WHITE), pos.square<KING> (BLACK));

        // Compute the initiative bonus for the attacking side
        i32 complexity =   8 * pe->asymmetry
                       +  12 * pos.count (PAWN)
                       +  12 * outflanking
                          // Pawn on both flanks
                       +  16 * (   0 != (pos.pieces (PAWN) & Side_bb[CS_KING])
                                && 0 != (pos.pieces (PAWN) & Side_bb[CS_QUEN]) ? 1 : 0)
                       +  48 * (VALUE_ZERO == pos.si->non_pawn_material () ? 1 : 0)
                       - 136;

        // Now apply the bonus: note that we find the attacking side by extracting
        // the sign of the endgame value, and that we carefully cap the bonus so
        // that the endgame score will never change sign after the bonus.
        auto score = mk_score (0, sign (eg) * std::max (complexity, -abs (eg)));

        if (Trace)
        {
            write (Term::INITIATIVE, score);
        }

        return score;
    }

    /// scale() evaluates the scale for the position
    template<bool Trace>
    Scale Evaluator<Trace>::scale (Value eg) const
    {
        auto color = eg >= VALUE_ZERO ? WHITE : BLACK;

        Scale scl;
        if (   nullptr == me->scale_func[color]
            || SCALE_NONE == (scl = (*me->scale_func[color])(pos)))
        {
            scl = me->scale[color];
        }
        assert(SCALE_NONE != scl);

        // If don't already have an unusual scale, check for certain types of endgames.
        if (SCALE_NORMAL == scl)
        {
            return pos.opposite_bishops ()
                && VALUE_MG_BSHP == pos.si->non_pawn_material (WHITE)
                && VALUE_MG_BSHP == pos.si->non_pawn_material (BLACK) ?
                    // Endings with opposite-colored bishops and no other pieces is almost a draw
                    Scale(31) :
                    std::min (Scale(40 + (pos.opposite_bishops () ? 2 : 7) * pos.count (color, PAWN)), SCALE_NORMAL);
        }
        return scl;
    }

    /// value() computes the various parts of the evaluation and
    /// returns the value of the position from the point of view of the side to move.
    template<bool Trace>
    Value Evaluator<Trace>::value ()
    {
        assert(0 == pos.si->checkers);

        // Probe the material hash table
        me = Material::probe (pos);
        // If have a specialized evaluation function for the material configuration
        if (nullptr != me->value_func)
        {
            return (*me->value_func) (pos);
        }

        // Probe the pawn hash table
        pe = Pawns::probe (pos);

        Score score;
        // Score is computed internally from the white point of view, initialize by
        // - incrementally updated scores (material + piece square tables).
        // - material imbalance.
        // - pawn score
        score = pos.psq
              + me->imbalance
              + pe->scores[WHITE]
              - pe->scores[BLACK]
              + pos.thread->contempt;

        // Early exit if score is high
        Value v = (mg_value (score) + eg_value (score)) / 2;
        if (abs (v) > LazyThreshold)
        {
            return WHITE == pos.active ? +v : -v;
        }

        if (Trace)
        {
            clear ();
        }

        initialize<WHITE> ();
        initialize<BLACK> ();

        // Pieces should be evaluated first (populate attack information)
        score += pieces<WHITE, NIHT> () - pieces<BLACK, NIHT> ()
               + pieces<WHITE, BSHP> () - pieces<BLACK, BSHP> ()
               + pieces<WHITE, ROOK> () - pieces<BLACK, ROOK> ()
               + pieces<WHITE, QUEN> () - pieces<BLACK, QUEN> ();

        assert((sgl_attacks[WHITE][NONE] & dbl_attacks[WHITE]) == dbl_attacks[WHITE]);
        assert((sgl_attacks[BLACK][NONE] & dbl_attacks[BLACK]) == dbl_attacks[BLACK]);

        score += mobility[WHITE] - mobility[BLACK];

        // Rest should be evaluated after (full attack information needed including king)
        score += king<WHITE> () - king<BLACK> ();

        score += threats<WHITE> () - threats<BLACK> ()
               + passers<WHITE> () - passers<BLACK> ();
        if (pos.si->non_pawn_material () >= SpaceThreshold)
        {
            score += space<WHITE> () - space<BLACK> ();
        }
        score += initiative (eg_value (score));

        assert(-VALUE_INFINITE < mg_value (score) && mg_value (score) < +VALUE_INFINITE);
        assert(-VALUE_INFINITE < eg_value (score) && eg_value (score) < +VALUE_INFINITE);
        assert(0 <= me->phase && me->phase <= Material::PhaseResolution);

        // Interpolates between midgame and scaled endgame values.
        v = Value(  (  mg_value (score) * (me->phase)
                     + eg_value (score) * (Material::PhaseResolution - me->phase) * scale (eg_value (score)) / SCALE_NORMAL)
                  / Material::PhaseResolution);

        if (Trace)
        {
            // Write remaining evaluation terms
            write (Term(PAWN), pe->scores[WHITE], pe->scores[BLACK]);
            write (Term::MATERIAL, pos.psq);
            write (Term::IMBALANCE, me->imbalance);
            write (Term::MOBILITY, mobility[WHITE], mobility[BLACK]);
            write (Term::TOTAL, score);
        }

        return (WHITE == pos.active ? +v : -v) + Tempo; // Side to move point of view
    }
}

/// evaluate() returns a static evaluation of the position from the point of view of the side to move.
Value evaluate (const Position &pos)
{
    return Evaluator<false> (pos).value ();
}

/// trace() returns a string (suitable for outputting to stdout) that contains
/// the detailed descriptions and values of each evaluation term.
string trace (const Position &pos)
{
    pos.thread->contempt = SCORE_ZERO; // Reset any dynamic contempt
    auto value = Evaluator<true> (pos).value ();
    value = WHITE == pos.active ? +value : -value; // Trace scores are from White's point of view

    ostringstream oss;
    oss << std::showpos << std::showpoint << std::setprecision (2) << std::fixed
        << "      Eval Term |    White    |    Black    |    Total     \n"
        << "                |   MG    EG  |   MG    EG  |   MG    EG   \n"
        << "----------------+-------------+-------------+--------------\n"
        << "       Material" << Term::MATERIAL
        << "      Imbalance" << Term::IMBALANCE
        << "     Initiative" << Term::INITIATIVE
        << "           Pawn" << Term(PAWN)
        << "         Knight" << Term(NIHT)
        << "         Bishop" << Term(BSHP)
        << "           Rook" << Term(ROOK)
        << "          Queen" << Term(QUEN)
        << "       Mobility" << Term::MOBILITY
        << "           King" << Term(KING)
        << "         Threat" << Term::THREAT
        << "    Pawn Passer" << Term::PASSER
        << "          Space" << Term::SPACE
        << "----------------+-------------+-------------+--------------\n"
        << "          Total" << Term::TOTAL
        << "\nEvaluation: " << value_to_cp (value) / 100.0 << " (white side)\n"
        << std::noshowpoint << std::noshowpos;
    return oss.str ();
}
