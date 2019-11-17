#include <sys/timerfd.h>
#include <sys/poll.h>
#include <iostream>
#include "blockstore.h"

class timerfd_interval
{
    int wait_state;
    int timerfd;
    int status;
    ring_loop_t *ringloop;
    ring_consumer_t consumer;
public:
    timerfd_interval(ring_loop_t *ringloop, int seconds)
    {
        wait_state = 0;
        timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (timerfd < 0)
        {
            throw std::runtime_error(std::string("timerfd_create: ") + strerror(errno));
        }
        struct itimerspec exp = {
            .it_interval = { seconds, 0 },
            .it_value = { seconds, 0 },
        };
        if (timerfd_settime(timerfd, 0, &exp, NULL))
        {
            throw std::runtime_error(std::string("timerfd_settime: ") + strerror(errno));
        }
        consumer.loop = [this]() { loop(); };
        ringloop->register_consumer(consumer);
        this->ringloop = ringloop;
    }

    ~timerfd_interval()
    {
        ringloop->unregister_consumer(consumer.number);
        close(timerfd);
    }

    void loop()
    {
        if (wait_state == 1)
        {
            return;
        }
        struct io_uring_sqe *sqe = ringloop->get_sqe();
        if (!sqe)
        {
            wait_state = 0;
            return;
        }
        struct ring_data_t *data = ((ring_data_t*)sqe->user_data);
        my_uring_prep_poll_add(sqe, timerfd, POLLIN);
        data->callback = [&](ring_data_t *data)
        {
            if (data->res < 0)
            {
                throw std::runtime_error(std::string("waiting for timer failed: ") + strerror(-data->res));
            }
            uint64_t n;
            read(timerfd, &n, 8);
            wait_state = 0;
            printf("tick 1s\n");
        };
        wait_state = 1;
        ringloop->submit();
    }
};

int main(int narg, char *args[])
{
    spp::sparse_hash_map<std::string, std::string> config;
    config["meta_device"] = "./test_meta.bin";
    config["journal_device"] = "./test_journal.bin";
    config["data_device"] = "./test_data.bin";
    ring_loop_t *ringloop = new ring_loop_t(512);
    // print "tick" every second
    timerfd_interval tick_tfd(ringloop, 1);
    while (true)
    {
        ringloop->loop(true);
    }
    //blockstore *bs = new blockstore(config, ringloop);
    
    //delete bs;
    delete ringloop;
    return 0;
}