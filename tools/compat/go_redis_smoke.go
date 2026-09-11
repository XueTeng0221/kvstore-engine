package main

import (
	"context"
	"fmt"
	"os"

	"github.com/redis/go-redis/v9"
)

func main() {
	ctx := context.Background()
	client := redis.NewClient(&redis.Options{Addr: "127.0.0.1:" + os.Args[1], Protocol: 2})
	defer client.Close()
	if err := client.Ping(ctx).Err(); err != nil {
		panic(err)
	}
	if err := client.Set(ctx, "go:key", "1", 0).Err(); err != nil {
		panic(err)
	}
	if value, err := client.Incr(ctx, "go:key").Result(); err != nil || value != 2 {
		panic(fmt.Sprintf("INCR: value=%d err=%v", value, err))
	}
	values, err := client.MGet(ctx, "go:key", "missing").Result()
	if err != nil || values[0] != "2" || values[1] != nil {
		panic(fmt.Sprintf("MGET: values=%v err=%v", values, err))
	}
	if value, err := client.Del(ctx, "go:key", "missing").Result(); err != nil || value != 1 {
		panic(fmt.Sprintf("DEL: value=%d err=%v", value, err))
	}
}
