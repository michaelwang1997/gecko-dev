/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */
"use strict";

const THREAD_TYPES = {
  MAIN_THREAD: "mainThread",
  CONTENT_PROCESS: "contentProcess",
  WORKER: "worker",
};

function registerThread(targetFront) {
  return async function(dispatch) {
    const threadFront = await targetFront.getFront("thread");
    const thread = {
      actor: threadFront.actor,
      url: targetFront.url,
      type: getTargetType(targetFront),
      name: targetFront.name,
      serviceWorkerStatus: targetFront.debuggerServiceWorkerStatus,

      // NOTE: target is used because when the target is destroyed
      // its ID and associated thread front are removed.
      _targetFront: targetFront,
    };

    dispatch({ type: "ADD_THREAD", thread });
  };
}

exports.clearThread = clearThread;
function clearThread(target) {
  return { type: "CLEAR_THREAD", target };
}

exports.selectThread = selectThread;
function selectThread(thread) {
  return function(dispatch) {
    dispatch({ type: "SELECT_THREAD", thread });
  };
}

function getTargetType(target) {
  if (target.isWorkerTarget) {
    return THREAD_TYPES.WORKER;
  }

  if (target.isContentProcess) {
    return THREAD_TYPES.CONTENT_PROCESS;
  }

  return THREAD_TYPES.MAIN_THREAD;
}

module.exports = {
  registerThread,
  clearThread,
  selectThread,
  THREAD_TYPES,
};
