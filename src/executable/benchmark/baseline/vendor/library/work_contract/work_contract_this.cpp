#include "./work_contract_this.h"


namespace bcpp
{

    thread_local this_contract * this_contract::tlsThisContract_ = nullptr;

} // namespace bcpp