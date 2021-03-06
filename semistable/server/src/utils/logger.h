/*
 *  The Mana Server
 *  Copyright (C) 2004-2010  The Mana World Development Team
 *
 *  This file is part of The Mana Server.
 *
 *  The Mana Server is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  The Mana Server is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Mana Server.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <iosfwd>
#include <sstream>

namespace utils
{

/**
 * A very simple logger that writes messages to a log file.
 * If the log file is not set, the messages are routed to the standard output
 * or the standard error depending on the level of the message.
 * By default, the messages will be timestamped but the logger can be
 * configured to not prefix the messages with a timestamp.
 *
 * Limitations:
 *     - not thread-safe.
 *
 * Example of use:
 *
 * <pre>
 * \#include "logger.h"
 *
 * int main()
 * {
 *     using namespace utils;
 *
 *     Logger::setLogFile("/path/to/logfile");
 *
 *     // log messages using helper macros.
 *     LOG_DEBUG("level: " << 3)
 *     LOG_INFO("init sound")
 *     LOG_WARN("not implemented")
 *     LOG_ERROR("resource not found")
 *     LOG_FATAL("unable to init graphics")
 *
 *     // log messages using APIs.
 *     std::ostringstream os;
 *     os << "level: " << 3;
 *     Logger::output(os.str(), Logger::Debug);
 *
 *     Logger::output("init sound", Logger::Info);
 *     Logger::output("not implemented", Logger::Warn);
 *     Logger::output("resource not found", Logger::Error);
 *     Logger::output("unable to init graphics", Logger::Fatal);
 *
 *     return 0;
 * }
 * </pre>
 */
class Logger
{
    public:
        enum Level
        {
            Fatal = 0,
            Error,
            Warn,
            Info,
            Debug
        };

        /**
         * Sets the log file.
         *
         * This method will open the log file for writing, the former file
         * contents are removed.
         *
         * @param logFile the log file name (may include path).
         *
         * @exception std::ios::failure if the log file could not be opened.
         */
        static void setLogFile(const std::string &logFile);

        /**
         * Add/removes the timestamp.
         *
         * @param flag if true, a log message will always be timestamped
         *             (default = true).
         */
        static void setTimestamp(bool flag = true)
        { mHasTimestamp = flag; }

        /**
         * Sets tee mode.
         *
         * @param flag if true, write messages to both the standard (or error)
         *        output and the log file (if set) (default = true).
         */
        static void setTeeMode(bool flag = true)
        { mTeeMode = flag; }

        /**
         * Sets the verbosity level of the logger.
         *
         * @param verbosity is the level of verbosity.
         */
        static void setVerbosity(Level verbosity)
        { mVerbosity = verbosity; }

        /**
         * Logs a generic message.
         *
         * @param msg the message to log.
         *
         * @param atVerbosity the minimum verbosity level
         *        to log this
         *
         * @exception std::ios::failure.
         */
        static void output(const std::string &msg, Level atVerbosity);

        static Level mVerbosity;   /**< Verbosity level. */
    private:
        static bool mHasTimestamp; /**< Timestamp flag. */
        static bool mTeeMode;      /**< Tee mode flag. */

        /**
         * Logs a generic message.
         *
         * @param os the output stream.
         * @param msg the message to log.
         * @param prefix the message prefix.
         *
         * @exception std::ios::failure.
         */
        static void output(std::ostream &os, const std::string &msg,
                           const char *prefix);
};


} // namespace utils

// HELPER MACROS

#define LOG(level, msg)                                                \
    do if (::utils::Logger::mVerbosity >= ::utils::Logger::level) {    \
        std::ostringstream os;                                         \
        os << msg;                                                     \
        ::utils::Logger::output(os.str(), ::utils::Logger::level);     \
    } while (0)

#define LOG_DEBUG(msg)  LOG(Debug, msg)
#define LOG_INFO(msg)   LOG(Info, msg)
#define LOG_WARN(msg)   LOG(Warn, msg)
#define LOG_ERROR(msg)  LOG(Error, msg)
#define LOG_FATAL(msg)  LOG(Fatal, msg)

#endif // LOGGER_H
