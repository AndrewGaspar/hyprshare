#pragma once

// Standard-conforming access to a private data member.
//
// Explicit template instantiations are exempt from access checking ([temp.spec.general]/6), so
// instantiating SPrivateMember with a pointer-to-private-member smuggles that pointer out through a
// friend function. Unlike `#define private public`, this cannot change the layout or the mangling of
// anything we include, which matters here: the plugin must stay ABI-identical to the running
// compositor.
//
// Usage:
//   struct SMyTag {
//       using type = int CFoo::*;
//       friend type privateMember(SMyTag);
//   };
//   template struct SPrivateMember<SMyTag, &CFoo::m_private>;
//   ...
//   int& x = foo->*privateMember(SMyTag{});

template <typename Tag, typename Tag::type MEMBER>
struct SPrivateMember {
    friend typename Tag::type privateMember(Tag) {
        return MEMBER;
    }
};

// Shorthand for the above: declares TAG and instantiates it for CLASS::MEMBER of type TYPE (use an
// alias for a TYPE containing commas). `obj->*privateMember(TAG{})` then reads/writes the member.
#define HYPRSHARE_PRIVATE_MEMBER(TAG, CLASS, TYPE, MEMBER)                                                                                                                         \
    struct TAG {                                                                                                                                                                   \
        using type = TYPE CLASS::*;                                                                                                                                                \
        friend type       privateMember(TAG);                                                                                                                                      \
    };                                                                                                                                                                             \
    template struct SPrivateMember<TAG, &CLASS::MEMBER>
