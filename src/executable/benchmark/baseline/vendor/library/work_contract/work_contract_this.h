#pragma once

#include "./work_contract_id.h"

#include <include/non_copyable.h>
#include <include/non_movable.h>


namespace bcpp
{

    class this_contract;


    struct alignas(64) this_contract :
        non_copyable,
        non_movable
    {

        this_contract
        (
            implementation::work_contract_id id,
            void * group,
            void(* release)(implementation::work_contract_id, void *),
            void(* schedule)(implementation::work_contract_id, void *)
        ) noexcept :
            prev_(tlsThisContract_),
            id_(id),
            group_(static_cast<void*>(group)),
            release_(release),
            schedule_(schedule)
        {
            tlsThisContract_ = this;
        }

        ~this_contract() noexcept
        {
            tlsThisContract_ = prev_;
        }

        static inline void schedule() noexcept {tlsThisContract_->schedule_(tlsThisContract_->id_, tlsThisContract_->group_);}

        static inline void release() noexcept {tlsThisContract_->release_(tlsThisContract_->id_, tlsThisContract_->group_);}

        static inline auto get_id() noexcept{return tlsThisContract_->id_;}

        static thread_local this_contract * tlsThisContract_;

        this_contract *                         prev_;
        implementation::work_contract_id        id_;
        void *                                  group_;

        void(* release_)(implementation::work_contract_id, void *);
        void(* schedule_)(implementation::work_contract_id, void *);

    }; // struct this_contract

} // namespace bcpp