#pragma once

#include "./work_contract_id.h"

#include <include/non_copyable.h>
#include <include/non_movable.h>


namespace bcpp
{

    //==============================================================================
    // gives the work function access to its own contract while it is executing.
    // the owning group is reached through an opaque pointer plus thunks so that
    // this type need not be templated on the group's subtree size.
    struct alignas(64) this_contract :
        non_copyable,
        non_movable
    {

        this_contract
        (
            work_contract_id id,
            void * group,
            void(* release)(work_contract_id, void *),
            void(* schedule)(work_contract_id, void *)
        ) noexcept :
            prev_(tlsThisContract_),
            id_(id),
            group_(group),
            release_(release),
            schedule_(schedule)
        {
            tlsThisContract_ = this;
        }

        ~this_contract() noexcept
        {
            tlsThisContract_ = prev_;
        }

        static void schedule() noexcept {tlsThisContract_->schedule_(tlsThisContract_->id_, tlsThisContract_->group_);}

        static void release() noexcept {tlsThisContract_->release_(tlsThisContract_->id_, tlsThisContract_->group_);}

        static auto get_id() noexcept {return tlsThisContract_->id_;}

        static bool is_executing() noexcept {return (tlsThisContract_ != nullptr);}

        static thread_local this_contract * tlsThisContract_;

        this_contract *     prev_;
        work_contract_id    id_;
        void *              group_;

        void(* release_)(work_contract_id, void *);
        void(* schedule_)(work_contract_id, void *);

    }; // struct this_contract

} // namespace bcpp
