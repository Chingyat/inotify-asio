#ifndef INOTIFY_ASIO_INOTIFY_HPP
#define INOTIFY_ASIO_INOTIFY_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/noncopyable.hpp>

#include <string>
#include <string_view>

#include <sys/inotify.h>

namespace inotify_asio 
{

using mask_type = std::uint32_t;
using cookie_type = std::uint32_t;

const static std::size_t min_buffer_size = sizeof(struct inotify_event) + NAME_MAX + 1;

class watch_item
{
public:
    watch_item(int wd, int fd)
        : wd_(wd)
        , fd_(fd)
    {}

    watch_item(watch_item &&other) noexcept
        : wd_(std::exchange(other.wd_, -1))
        , fd_(std::exchange(other.fd_, -1))
    {}

    ~watch_item() noexcept
    {
        ::inotify_rm_watch(fd_, wd_);
    }

    int fd() const
    {
        return fd_;
    }

    int wd() const
    {
        return wd_;
    }

    void forget()
    {
        wd_ = -1;
    }

private:
    int wd_;
    int fd_;
};

class event
{
public:
    event() = default;

    explicit event(const struct inotify_event* ev)
        : wd_(ev->wd),
          mask_(ev->mask),
          cookie_(ev->cookie),
          name_(ev->name, ev->len ? ev->len - 1 : 0)
    {}

    int wd() const
    {
        return wd_;
    }

    mask_type mask() const
    {
        return mask_;
    }

    cookie_type cookie() const
    {
        return cookie_;
    }

    std::string name() const
    {
        return name_;
    }

private:
    uint32_t wd_;
    uint32_t mask_;
    uint32_t cookie_;
    std::string name_;
};


class inotify
{
public:
    explicit inotify(boost::asio::io_context &ioc)
        : desc_(ioc)
    {
        int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0)
        {
            throw boost::system::system_error(errno, boost::system::system_category(), "inotify::inotify");
        }
        desc_.assign(fd);
    }

    watch_item add(const std::string &pathname, mask_type mask, boost::system::error_code &ec)
    {
        int wd = ::inotify_add_watch(desc_.native_handle(), pathname.c_str(), mask);
        if (wd < 0)
        {
            ec.assign(errno, boost::system::system_category());
        }
        else
        {
            ec.clear();
        }
        return {wd, desc_.native_handle()};
    }

    watch_item add(const std::string &pathname, mask_type mask)
    {
        boost::system::error_code ec;
        auto ret = add(pathname, mask, ec);
        if (ec)
        {
            throw boost::system::system_error(ec, "inotify::add");
        }
        return ret;
    }

    event watch(boost::system::error_code &ec)
    {
        if (buffer_.size() == 0)
        {
            std::size_t bytes = desc_.read_some(buffer_.prepare(min_buffer_size), ec);
            if (ec)
            {
                return {};
            }
            buffer_.commit(bytes);
        }

        return extract_event(buffer_);
    }

    event watch()
    {
        boost::system::error_code ec;
        auto ret = watch(ec);
        if (ec)
        {
            throw boost::system::system_error(ec, "inotify::watch");
        }
        return ret;
    }

    template <typename CompletionToken>
    boost::asio::async_result<void(boost::system::error_code), CompletionToken>
    async_watch(CompletionToken &&token)
    {
        auto initiation = [](auto &&completion_handler,
            boost::asio::posix::stream_descriptor& desc,
            boost::beast::flat_buffer& buffer)
        {
            struct intermediate_handler : boost::asio::coroutine
            {
                boost::asio::posix::stream_descriptor &desc_;
                boost::beast::flat_buffer& buffer_;
                std::decay_t<decltype(completion_handler)> handler_;

                bool need_read;

#include <boost/asio/yield.hpp>
                void operator()(
                    boost::system::error_code ec = {}, 
                    std::size_t bytes_transferred = 0)
                {
                    reenter(this)
                    {
                        if (need_read)
                        {
                            yield desc_.async_read_some(buffer_.prepare(min_buffer_size), std::move(*this));

                            if (ec)
                            {
                                handler_(ec, event{});
                                yield return;
                            }
                        }
                        else
                        {
                            yield boost::asio::post(desc_.get_executor(), std::move(*this));
                        }

                        handler_(ec, extract_event(buffer_));
                        yield return;
                    }
                }
#include <boost/asio/unyield.hpp>
            };

            intermediate_handler{
                {},
                desc,
                buffer,
                completion_handler,
                buffer.size() == 0
            }();
        };

        boost::asio::async_initiate<
            CompletionToken, 
            void(boost::system::error_code, event)>(
                initiation, token, desc_, buffer_);
    }

private:
    static event extract_event(boost::beast::flat_buffer &buffer)
    {
        const struct inotify_event *evbuf = static_cast<const struct inotify_event *>(
            buffer.data().data());
        event ev(evbuf);

        buffer.consume(sizeof(struct inotify_event) + evbuf->len);
        return ev;
    }

    boost::asio::posix::stream_descriptor desc_;
    boost::beast::flat_buffer buffer_;
};

}

#endif // INOTIFY_ASIO_INOTIFY_HPP