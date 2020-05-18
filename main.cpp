// modified official example of http-crawler from github.com/boostorg/beast/tree/develop/example/http/client/crawl
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <fstream>

#include <gumbo.h>

#include "gumbo_utils.hpp"

#include "thread_safe_queue.hpp"


namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = net::ip::tcp;               // from <boost/asio/ip/tcp.hpp>

//------------------------------------------------------------------------------


// This structure aggregates statistics on all the sites
class crawl_report {

    net::io_context &ioc_;
    net::strand<net::io_context::executor_type> strand_;

    std::size_t count_ = 0;
    std::size_t sz = 0;

public:
    explicit crawl_report(net::io_context &ioc, size_t s)
            : ioc_(ioc), strand_(ioc_.get_executor()), sz(s) {
    }

    // Run an aggregation function on the strand.
    // This allows synchronization without a mutex.
    template<class F>
    void aggregate(F const &f) {
        net::post(
                strand_,
                [&, f] {
                    f(*this);
                    if (count_ % 10 == 0) {
                        std::cerr << "Progress: " << count_ << " of " << sz << "\n";
                    }
                    ++count_;
                });
    }

    // Counts the number of timer failures
    std::size_t timer_failures = 0;

    // Counts the number of name resolution failures
    std::size_t resolve_failures = 0;

    // Counts the number of connection failures
    std::size_t connect_failures = 0;

    // Counts the number of write failures
    std::size_t write_failures = 0;

    // Counts the number of read failures
    std::size_t read_failures = 0;

    // Counts the number of success reads
    std::size_t success = 0;

    // Counts the number received of each status code
    std::map<unsigned, std::size_t> status_codes;
};

std::ostream &operator<<(std::ostream &os, crawl_report const &report) {
    // Print the report
    os <<
       "Crawl report\n" <<
       "   Failure counts\n" <<
       "       Timer   : " << report.timer_failures << "\n" <<
       "       Resolve : " << report.resolve_failures << "\n" <<
       "       Connect : " << report.connect_failures << "\n" <<
       "       Write   : " << report.write_failures << "\n" <<
       "       Read    : " << report.read_failures << "\n" <<
       "       Success : " << report.success << "\n" <<
       "   Status codes\n";
    for (auto const &result : report.status_codes) {
        os << "       " << std::setw(3) << result.first << ": " << result.second << " ("
           << http::obsolete_reason(static_cast<http::status>(result.first)) << ")\n";
    }
    os.flush();
    return os;
}

//------------------------------------------------------------------------------

// Performs HTTP GET requests and aggregates the results into a report
class worker : public std::enable_shared_from_this<worker> {
    crawl_report &report_;
    tcp::resolver resolver_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_; // (Must persist between reads)
    http::request<http::empty_body> req_;
    http::response<http::string_body> res_;
    thread_safe_queue<std::string> &uqueue;
    size_t max_pages;
    size_t pages_indexed = 0;
public:
    worker(worker &&) = default;

    // Resolver and socket require an io_context
    worker(crawl_report &report, thread_safe_queue<std::string> &uq, size_t mp, net::io_context &ioc) : report_(report),
                                                                                                        resolver_(
                                                                                                                net::make_strand(
                                                                                                                        ioc)),
                                                                                                        uqueue(uq),
                                                                                                        max_pages(mp),
                                                                                                        stream_(net::make_strand(
                                                                                                                ioc)) {
        // Set up the common fields of the request
        req_.version(11);
        req_.method(http::verb::get);
        req_.target("/");
        req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    }

    // Start the asynchronous operation
    void run() { do_get_host(); }

    void do_get_host() {
        if (pages_indexed >= max_pages) return;

        // Grab another host
        std::string host;
        uqueue.pop(host);

        // The Host HTTP field is required
        req_.set(http::field::host, host);
        // Set up an HTTP GET request message
        // Look up the domain name
        resolver_.async_resolve(host, "http", beast::bind_front_handler(&worker::on_resolve, shared_from_this()));
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        ++pages_indexed;
        if (ec) {
            report_.aggregate(
                    [](crawl_report &rep) {
                        ++rep.resolve_failures;
                    });
            return do_get_host();
        }
        // Set a timeout on the operation
        stream_.expires_after(std::chrono::seconds(10));
        // See IP address from a lookup
        /*for (auto &v: results) {
            std::cout << v.endpoint() << std::endl;
            std::cout << v.host_name() << std::endl;
            std::cout << v.service_name() << std::endl;
        }*/
        // Make the connection on the IP address we get from a lookup
        stream_.async_connect(results, beast::bind_front_handler(&worker::on_connect, shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) {
            report_.aggregate(
                    [](crawl_report &rep) {
                        ++rep.connect_failures;
                    });
            return do_get_host();
        }
        // Set a timeout on the operation
        stream_.expires_after(std::chrono::seconds(10));
        // Send the HTTP request to the remote host
        http::async_write(stream_, req_, beast::bind_front_handler(&worker::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            report_.aggregate(
                    [](crawl_report &rep) {
                        ++rep.write_failures;
                    });
            return do_get_host();
        }
        // Receive the HTTP response
        res_ = {};
        http::async_read(stream_, buffer_, res_, beast::bind_front_handler(&worker::on_read, shared_from_this()));
    }

    void extract_all_urls(std::string &code) {
        EasyGumbo::Gumbo parser(code.c_str());
        EasyGumbo::Gumbo::iterator iter = parser.begin();
        while (iter != parser.end()) {
            iter = std::find_if(iter, parser.end(), EasyGumbo::Tag(GUMBO_TAG_A));
            if (iter == parser.end()) {
                break;
            }
            EasyGumbo::Element titleA(*iter);
            // std::cout << "***\n";
            auto url = std::string(titleA.attribute("href")->value);
            if (url.substr(0, 3) == "htt") {

                auto index = url.find("/") + 2;
                auto value_to_push = url.substr(index, url.size() - index);

//                std::cout << std::setw(8) << "Url" << " : " << value_to_push << std::endl;

                uqueue.push(value_to_push);
            }

            ++iter;
        }

    }


    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            report_.aggregate(
                    [](crawl_report &rep) {
                        ++rep.read_failures;
                    });
            return do_get_host();
        }
        auto const code = res_.result_int();
        report_.aggregate(
                [code](crawl_report &rep) {
                    ++rep.success;
                    ++rep.status_codes[code];

                });

        std::string response_body = res_.body();

        GumboOutput *output = gumbo_parse(response_body.c_str());

        extract_all_urls(response_body);

        gumbo_destroy_output(&kGumboDefaultOptions, output);

        // Gracefully close the socket
        stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
        stream_.close();
        // If we get here then the connection is closed gracefully
        do_get_host();
    }
};

inline std::chrono::high_resolution_clock::time_point now() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto res_time = std::chrono::high_resolution_clock::now();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return res_time;
}

template<class D>
inline long long duration(const D &d) {
    return std::chrono::duration_cast<std::chrono::seconds>(d).count();
}


std::vector<char const *> const &urls_large_data() {
    static std::vector<char const *> const urls({"go.com"});
    return urls;
}


class config {
public:
    std::string seed_webpage{};
    size_t max_pages = 0;
    size_t threads_count = 0;
};

void read_config(std::string &&filename, config &cfg) {
    std::ifstream cf(filename);
    std::string temp;
    if (!cf.is_open()) {
        std::cerr << "Failed to open configuration file " << filename << std::endl;
        return;
    }
    cf >> cfg.seed_webpage;
    getline(cf, temp);
    cf >> cfg.threads_count;
    getline(cf, temp);
    cf >> cfg.max_pages;
}


int main(int argc, char *argv[]) {
    // Check command line arguments.
    if (argc != 2) {
        std::cerr <<
                  "Usage: web-crawler <config-path>\n" <<
                  "Example:\n" <<
                  "    web-crawler conf.txt\n";
        return -1;
    }


    config cfg;

    read_config(std::string(argv[1]), cfg);

    thread_safe_queue<std::string> url_queue;
    url_queue.push(cfg.seed_webpage);
    auto const threads = cfg.threads_count;
    // The io_context is required for all I/O
    net::io_context ioc;
    // The work keeps io_context::run from returning
    auto work = net::make_work_guard(ioc);
    // The report holds the aggregated statistics
    crawl_report report{ioc, cfg.max_pages * cfg.threads_count};

    // Create and launch the worker threads.
    std::vector<std::thread> workers;
    workers.reserve(threads + 1);
    auto s = now();
    for (int i = 0; i < threads; ++i)
        workers.emplace_back(
                [&report, &url_queue, &cfg] {
                    // We use a separate io_context for each worker because
                    // the asio resolver simulates asynchronous operation using
                    // a dedicated worker thread per io_context, and we want to
                    // do a lot of name resolutions in parallel.
                    net::io_context ioc{1};
                    std::make_shared<worker>(report, url_queue, cfg.max_pages, ioc)->run();
                    ioc.run();
                });
    // Add another thread to run the main io_context which
    // is used to aggregate the statistics
    workers.emplace_back(
            [&ioc] {
                ioc.run();
            });
    // Now block until all threads exit
    for (std::size_t i = 0; i < workers.size(); ++i) {
        auto &thread = workers[i];
        // If this is the last thread, reset the
        // work object so that it can return from run.
        if (i == workers.size() - 1)
            work.reset();
        // Wait for the thread to exit
        thread.join();
    }
    auto f = now();
    std::cout << "Elapsed time:    " << duration(f - s) << " seconds\n";
    std::cout << report;
    return 0;
}
