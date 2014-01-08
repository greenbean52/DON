#include "Material.h"
#include <algorithm>
#include <cassert>
#include <cstring>

using namespace std;
using namespace EndGame;

namespace {

    // Values modified by Joona Kiiski
    const Value MidgameLimit = Value (15581);
    const Value EndgameLimit = Value (3998);

    // Scale factors used when one side has no more pawns
    const int32_t NoPawnsSF[4] = {  6, 12, 32,  0, };

    // Polynomial material balance parameters
    //                                            P      N      B      R      Q     BP
    const int32_t LinearCoefficients[PT_NO] = { -162, -1122,  -183,   249,   -52,  1852, };

    const int32_t QuadraticCoefficientsSameColor[PT_NO][PT_NO] =
    {
        // P    N    B    R    Q    BP
        {   2,   0,   0,   0,   0,   0, }, // P
        { 271,  -4,   0,   0,   0,   0, }, // N
        { 105,   4,   0,   0,   0,   0, }, // B
        {  -2,  46, 100,-141,   0,   0, }, // R
        {  29,  83, 148,-163,   0,   0, }, // Q
        {  39,  35,   0, -27,  58,   0, }, // BP
    };

    const int32_t QuadraticCoefficientsOppositeColor[PT_NO][PT_NO] =
    {
        //       THEIR PIECES
        // P    N    B    R    Q    BP
        {   0,   0,   0,   0,   0,   0, }, // P
        {  62,   0,   0,   0,   0,   0, }, // N
        {  64,  39,   0,   0,   0,   0, }, // B     OUR PIECES
        {  40,  23, -22,   0,   0,   0, }, // R
        { 101,   3, 151, 171,   0,   0, }, // Q
        {  37,  10,  57,  50, 106,   0, }, // BP
    };

    // Endgame evaluation and scaling functions accessed direcly and not through
    // the function maps because correspond to more than one material hash key.
    Endgame<KmmKm> EvaluateKmmKm[CLR_NO] = { Endgame<KmmKm>  (WHITE), Endgame<KmmKm>  (BLACK) };
    Endgame<KXK>   EvaluateKXK  [CLR_NO] = { Endgame<KXK>    (WHITE), Endgame<KXK>    (BLACK) };

    Endgame<KBPsK>  ScaleKBPsK  [CLR_NO] = { Endgame<KBPsK>  (WHITE), Endgame<KBPsK>  (BLACK) };
    Endgame<KQKRPs> ScaleKQKRPs [CLR_NO] = { Endgame<KQKRPs> (WHITE), Endgame<KQKRPs> (BLACK) };
    Endgame<KPsK>   ScaleKPsK   [CLR_NO] = { Endgame<KPsK>   (WHITE), Endgame<KPsK>   (BLACK) };
    Endgame<KPKP>   ScaleKPKP   [CLR_NO] = { Endgame<KPKP>   (WHITE), Endgame<KPKP>   (BLACK) };

    // Helper templates used to detect a given material distribution
    template<Color C> bool is_KXK(const Position &pos)
    {
        const Color C_ = ((WHITE == C) ? BLACK : WHITE);
        return  !pos.piece_count<PAWN> (C_)
            && pos.non_pawn_material (C_) == VALUE_ZERO
            && pos.non_pawn_material (C) >= VALUE_MG_ROOK;
    }

    template<Color C> bool is_KBPsKs(const Position &pos)
    {
        return pos.non_pawn_material (C) == VALUE_MG_BISHOP
            && pos.piece_count<BSHP> (C) == 1
            && pos.piece_count<PAWN> (C) >= 1;
    }

    template<Color C> bool is_KQKRPs(const Position &pos)
    {
        const Color C_  = ((WHITE == C) ? BLACK : WHITE);
        
        return!pos.piece_count<PAWN> (C)
            && pos.non_pawn_material (C) == VALUE_MG_QUEEN
            && pos.piece_count<QUEN> (C)  == 1
            && pos.piece_count<ROOK> (C_) == 1
            && pos.piece_count<PAWN> (C_) >= 1;
    }

    template<Color C>
    // imbalance<> () calculates imbalance comparing piece count of each
    // piece type for both colors.
    int32_t imbalance (const int32_t piece_count[][PT_NO])
    {
        const Color C_  = ((WHITE == C) ? BLACK : WHITE);

        int32_t value = VALUE_ZERO;

        // "The Evaluation of Material Imbalances in Chess"

        // Second-degree polynomial material imbalance by Tord Romstad
        for (PType pt1 = PAWN; pt1 <= KING; ++pt1)
        {
            int32_t pc = piece_count[C][pt1];
            if (!pc) continue;

            int32_t v = LinearCoefficients[pt1];

            for (PType pt2 = PAWN; pt2 <= pt1; ++pt2)
            {
                v += QuadraticCoefficientsSameColor    [pt1][pt2] * piece_count[C ][pt2]
                +    QuadraticCoefficientsOppositeColor[pt1][pt2] * piece_count[C_][pt2];
            }

            value += pc * v;
        }
        return value;
    }

} // namespace

namespace Material {

    // Material::probe () takes a position object as input, looks up a MaterialEntry
    // object, and returns a pointer to it. If the material configuration is not
    // already present in the table, it is computed and stored there, so we don't
    // have to recompute everything when the same material configuration occurs again.
    Entry* probe (const Position &pos, Table &table, Endgames &endgames)
    {
        Key key  = pos.matl_key ();
        Entry *e = table[key];

        // If e->key matches the position's material hash key, it means that we
        // have analysed this material configuration before, and we can simply
        // return the information we found the last time instead of recomputing it.
        if (e->_key == key) return e;

        memset (e, 0, sizeof (Entry));
        e->_key           = key;
        e->_factor[WHITE] = e->_factor[BLACK] = SCALE_FACTOR_NORMAL;
        e->_game_phase    = game_phase (pos);

        // Let's look if we have a specialized evaluation function for this
        // particular material configuration. First we look for a fixed
        // configuration one, then a generic one if previous search failed.
        if (endgames.probe (key, e->evaluation_func))
        {
            return e;
        }
        if (is_KXK<WHITE> (pos))
        {
            e->evaluation_func = &EvaluateKXK[WHITE];
            return e;
        }
        if (is_KXK<BLACK> (pos))
        {
            e->evaluation_func = &EvaluateKXK[BLACK];
            return e;
        }

        if (!pos.pieces (PAWN) && !pos.pieces (ROOK) && !pos.pieces (QUEN))
        {
            // Minor piece endgame with at least one minor piece per side and
            // no pawns. Note that the case KmmK is already handled by KXK.
            ASSERT ((pos.pieces (WHITE, NIHT) | pos.pieces (WHITE, BSHP)));
            ASSERT ((pos.pieces (BLACK, NIHT) | pos.pieces (BLACK, BSHP)));

            if (   pos.piece_count<BSHP> (WHITE) + pos.piece_count<NIHT> (WHITE) <= 2
                && pos.piece_count<BSHP> (BLACK) + pos.piece_count<NIHT> (BLACK) <= 2)
            {
                e->evaluation_func = &EvaluateKmmKm[pos.active ()];
                return e;
            }
        }

        // OK, we didn't find any special evaluation function for the current
        // material configuration. Is there a suitable scaling function?
        //
        // We face problems when there are several conflicting applicable
        // scaling functions and we need to decide which one to use.
        EndgameBase<ScaleFactor> *eg_sf;
        if (endgames.probe (key, eg_sf))
        {
            e->scaling_func[eg_sf->color ()] = eg_sf;
            return e;
        }

        // Generic scaling functions that refer to more then one material
        // distribution. Should be probed after the specialized ones.
        // Note that these ones don't return after setting the function.
        if (is_KBPsKs<WHITE> (pos))
        {
            e->scaling_func[WHITE] = &ScaleKBPsK[WHITE];
        }
        if (is_KBPsKs<BLACK> (pos))
        {
            e->scaling_func[BLACK] = &ScaleKBPsK[BLACK];
        }
        if (is_KQKRPs<WHITE> (pos))
        {
            e->scaling_func[WHITE] = &ScaleKQKRPs[WHITE];
        }
        if (is_KQKRPs<BLACK> (pos))
        {
            e->scaling_func[BLACK] = &ScaleKQKRPs[BLACK];
        }

        Value w_npm = pos.non_pawn_material (WHITE);
        Value b_npm = pos.non_pawn_material (BLACK);

        if (w_npm + b_npm == VALUE_ZERO)
        {
            if (!pos.piece_count<PAWN> (BLACK))
            {
                ASSERT (pos.piece_count<PAWN> (WHITE) >= 2);
                e->scaling_func[WHITE] = &ScaleKPsK[WHITE];
            }
            else if (!pos.piece_count<PAWN> (WHITE))
            {
                ASSERT (pos.piece_count<PAWN> (BLACK) >= 2);
                e->scaling_func[BLACK] = &ScaleKPsK[BLACK];
            }
            else if (pos.piece_count<PAWN> (WHITE) == 1 && pos.piece_count<PAWN> (BLACK) == 1)
            {
                // This is a special case because we set scaling functions for both colors instead of only one.
                e->scaling_func[WHITE] = &ScaleKPKP[WHITE];
                e->scaling_func[BLACK] = &ScaleKPKP[BLACK];
            }
        }

        // No pawns makes it difficult to win, even with a material advantage.
        // This catches some trivial draws like KK, KBK and KNK
        if (pos.piece_count<PAWN> (WHITE) == 0 && w_npm - b_npm <= VALUE_MG_BISHOP)
        {
            e->_factor[WHITE] = (w_npm == b_npm || w_npm < VALUE_MG_ROOK ? 0 : NoPawnsSF[min (pos.piece_count<BSHP> (WHITE), 2)]);
        }
        if (pos.piece_count<PAWN> (BLACK) == 0 && b_npm - w_npm <= VALUE_MG_BISHOP)
        {
            e->_factor[BLACK] = (w_npm == b_npm || b_npm < VALUE_MG_ROOK ? 0 : NoPawnsSF[min (pos.piece_count<BSHP> (BLACK), 2)]);
        }

        if (pos.piece_count<PAWN> (WHITE) == 1 && w_npm - b_npm <= VALUE_MG_BISHOP)
        {
            e->_factor[WHITE] = uint8_t (SCALE_FACTOR_ONEPAWN);
        }
        if (pos.piece_count<PAWN> (BLACK) == 1 && b_npm - w_npm <= VALUE_MG_BISHOP)
        {
            e->_factor[BLACK] = uint8_t (SCALE_FACTOR_ONEPAWN);
        }

        // Compute the space weight
        if (w_npm + b_npm >= 2 * VALUE_MG_QUEEN + 4 * VALUE_MG_ROOK + 2 * VALUE_MG_KNIGHT)
        {
            int32_t minor_piece_count = pos.piece_count<NIHT> () + pos.piece_count<BSHP> ();

            e->_space_weight = mk_score (minor_piece_count * minor_piece_count, 0);
        }

        // Evaluate the material imbalance.
        // We use KING as a place holder for the bishop pair "extended piece",
        // this allow us to be more flexible in defining bishop pair bonuses.
        const int32_t piece_count[CLR_NO][PT_NO] =
        {
            {pos.piece_count<PAWN> (WHITE), pos.piece_count<NIHT> (WHITE), pos.piece_count<BSHP> (WHITE),
            pos.piece_count<ROOK> (WHITE), pos.piece_count<QUEN> (WHITE), pos.piece_count<BSHP> (WHITE) > 1
            },
            {pos.piece_count<PAWN> (BLACK), pos.piece_count<NIHT> (BLACK), pos.piece_count<BSHP> (BLACK),
            pos.piece_count<ROOK> (BLACK), pos.piece_count<QUEN> (BLACK), pos.piece_count<BSHP> (BLACK) > 1,
            },
        };

        e->_value = int16_t ((imbalance<WHITE> (piece_count) - imbalance<BLACK> (piece_count)) / 16);
        return e;
    }

    // Material::game_phase () calculates the phase given the current position.
    // Because the phase is strictly a function of the material, it is stored in MaterialEntry.
    Phase game_phase (const Position &pos)
    {
        Value npm = pos.non_pawn_material (WHITE) + pos.non_pawn_material (BLACK);

        return npm >= MidgameLimit ? PHASE_MIDGAME
            :  npm <= EndgameLimit ? PHASE_ENDGAME
            :  Phase (((npm - EndgameLimit) * 128) / (MidgameLimit - EndgameLimit));
    }

} // namespace Material
