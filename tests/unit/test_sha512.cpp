// SHA-512 and the "$6$" password hash, against the published vectors (FIPS
// 180-4; Drepper's SHA-crypt specification). A Luban system's user password
// is hashed in-process with these, never by an external program.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.sha512;

XTEST(Sha512, MatchesThePublishedVectors, .area = "libs", .covers = {"USER-PASSWORD"}) {
    EXPECT_EQ(xlings::sha512::hex("abc"),
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    EXPECT_EQ(xlings::sha512::hex(""),
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    // Two blocks.
    EXPECT_EQ(xlings::sha512::hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                  "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
}

XTEST(Sha512, CryptMatchesTheSpecificationsVectors, .area = "libs", .covers = {"USER-PASSWORD"}) {
    EXPECT_EQ(xlings::sha512::crypt("Hello world!", "saltstring"),
              "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1");
    EXPECT_EQ(xlings::sha512::crypt("Hello world!", "saltstringsaltstring", 10000),
              "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.");
    EXPECT_EQ(xlings::sha512::crypt("we have a short salt string but not a short password", "short", 77777),
              "$6$rounds=77777$short$WuQyW2YR.hBNpjjRhpYD/ifIw05xdfeEyQoMxIXbkvr0gge1a1x3yRULJ5CCaUeOxFmtlcGZelFl5CxtgfiAc0");
    const auto salt = xlings::sha512::random_salt();
    EXPECT_EQ(salt.size(), 16u);
    EXPECT_NE(salt, xlings::sha512::random_salt());
}
