/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <asio.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

#include "mongo/base/status.h"
#include "mongo/client/connection_pool.h"
#include "mongo/client/remote_command_runner.h"
#include "mongo/executor/network_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/protocol.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace executor {

/**
 * Implementation of the replication system's network interface using Christopher
 * Kohlhoff's ASIO library instead of existing MongoDB networking primitives.
 */
class NetworkInterfaceASIO final : public NetworkInterface {
public:
    NetworkInterfaceASIO();
    std::string getDiagnosticString() override;
    std::string getHostName() override;
    void startup() override;
    void shutdown() override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    void startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                      const RemoteCommandRequest& request,
                      const RemoteCommandCompletionFn& onFinish) override;
    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) override;

    bool inShutdown() const;

private:
    using ResponseStatus = TaskExecutor::ResponseStatus;
    using NetworkInterface::RemoteCommandCompletionFn;

    enum class State { kReady, kRunning, kShutdown };

    /**
     * AsyncConnection encapsulates the per-connection state we maintain.
     */
    class AsyncConnection {
    public:
        AsyncConnection(asio::ip::tcp::socket&& sock, rpc::ProtocolSet serverProtocols);

        AsyncConnection(asio::ip::tcp::socket&& sock,
                        rpc::ProtocolSet serverProtocols,
                        boost::optional<ConnectionPool::ConnectionPtr>&& bootstrapConn);

        asio::ip::tcp::socket& sock();

        rpc::ProtocolSet serverProtocols() const;
        rpc::ProtocolSet clientProtocols() const;

// Explicit move construction and assignment to support MSVC
#if defined(_MSC_VER) && _MSC_VER < 1900
        AsyncConnection(AsyncConnection&&);
        AsyncConnection& operator=(AsyncConnection&&);
#else
        AsyncConnection(AsyncConnection&&) = default;
        AsyncConnection& operator=(AsyncConnection&&) = default;
#endif

    private:
        asio::ip::tcp::socket _sock;

        rpc::ProtocolSet _serverProtocols;
        rpc::ProtocolSet _clientProtocols{rpc::supports::kAll};

        /**
         * The bootstrap connection we use to run auth. This will eventually go away when we finish
         * implementing async auth, but for now we need to keep it alive so that the socket it
         * creates stays open.
         */
        boost::optional<ConnectionPool::ConnectionPtr> _bootstrapConn;
    };

    /**
     * Helper object to manage individual network operations.
     */
    class AsyncOp {
    public:
        AsyncOp(const TaskExecutor::CallbackHandle& cbHandle,
                const RemoteCommandRequest& request,
                const RemoteCommandCompletionFn& onFinish,
                Date_t now,
                int id);

        std::string toString() const;

        void cancel();
        bool canceled() const;

        const TaskExecutor::CallbackHandle& cbHandle() const;

        AsyncConnection* connection();

        void connect(ConnectionPool* const pool, asio::io_service* service, Date_t now);
        void setConnection(AsyncConnection&& conn);
        bool connected() const;

        void finish(const TaskExecutor::ResponseStatus& status);

        MSGHEADER::Value* header();

        const RemoteCommandRequest& request() const;

        Date_t start() const;

        Message* toSend();

        void setToSend(Message&& message);

        Message* toRecv();

        rpc::Protocol operationProtocol() const;

        void setOperationProtocol(rpc::Protocol proto);

    private:
        enum class OpState {
            kReady,
            kConnectionAcquired,
            kConnectionVerified,
            kConnected,
            kCompleted
        };

        // Information describing an in-flight command.
        TaskExecutor::CallbackHandle _cbHandle;
        RemoteCommandRequest _request;
        RemoteCommandCompletionFn _onFinish;

        /**
         * The connection state used to service this request. We wrap it in an optional
         * as it is instantiated at some point after the AsyncOp is created.
         */
        boost::optional<AsyncConnection> _connection;

        /**
         * The RPC protocol used for this operation. We wrap it in an optional as it
         * is not known until we obtain a connection.
         */
        boost::optional<rpc::Protocol> _operationProtocol;

        const Date_t _start;

        OpState _state;
        AtomicUInt64 _canceled;

        /**
         * The outgoing command associated with this operation.
         */
        boost::optional<Message> _toSend;

        Message _toRecv;
        MSGHEADER::Value _header;

        const int _id;
    };

    void _asyncRunCommand(AsyncOp* op);

    std::unique_ptr<Message> _messageFromRequest(const RemoteCommandRequest& request,
                                                 rpc::Protocol protocol);

    void _asyncSendSimpleMessage(AsyncOp* op, const asio::const_buffer& buf);

    // Connection
    void _connectASIO(AsyncOp* op);
    void _connectWithDBClientConnection(AsyncOp* op);
    void _setupSocket(AsyncOp* op, const asio::ip::tcp::resolver::iterator& endpoints);
    void _authenticate(AsyncOp* op);
    void _sslHandshake(AsyncOp* op);

    // Communication state machine
    void _beginCommunication(AsyncOp* op);
    void _completedWriteCallback(AsyncOp* op);
    void _networkErrorCallback(AsyncOp* op, const std::error_code& ec);

    void _completeOperation(AsyncOp* op, const TaskExecutor::ResponseStatus& resp);

    void _recvMessageHeader(AsyncOp* op);
    void _recvMessageBody(AsyncOp* op);
    void _receiveResponse(AsyncOp* op);

    void _signalWorkAvailable_inlock();

    asio::io_service _io_service;
    stdx::thread _serviceRunner;

    asio::ip::tcp::resolver _resolver;

    std::atomic<State> _state;

    stdx::mutex _inProgressMutex;
    std::unordered_map<AsyncOp*, std::unique_ptr<AsyncOp>> _inProgress;

    stdx::mutex _executorMutex;
    bool _isExecutorRunnable;
    stdx::condition_variable _isExecutorRunnableCondition;

    std::unique_ptr<ConnectionPool> _connPool;

    AtomicUInt64 _numOps;
};

}  // namespace executor
}  // namespace mongo
