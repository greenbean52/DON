#ifndef _EVALUATOR_H_INC_
#define _EVALUATOR_H_INC_

#include "Type.h"

#include "Position.h"

namespace Evaluator {

    // Tempo bonus
    const Value Tempo = Value(20);

    template<bool Trace = false>
    extern Value evaluate (const Position &pos);
    
    extern std::string trace (const Position &pos);
    
    extern void initialize ();
}

#endif // _EVALUATOR_H_INC_
