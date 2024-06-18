#pragma once

enum class Command : unsigned char {
    select,
    del,
    exists,
    move,
    rename,
    renamenx,
    type,
    set,
    get,
    getRange,
    getBit,
    setBit,
    mget,
    setnx,
    setRange,
    strlen,
    mset,
    msetnx,
    incr,
    incrBy,
    decr,
    decrBy,
    append,
    hdel,
    hexists,
    hget,
    hgetAll,
    hincrBy,
    hkeys,
    hlen,
    hset,
    hvals,
    lindex,
    llen,
    lpop,
    lpush,
    lpushx
};
