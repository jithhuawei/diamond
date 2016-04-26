#!/usr/bin/python

import argparse
import os
import re
import redis
import sys

parser = argparse.ArgumentParser(description='Parse caching client results.')
parser.add_argument('--directory', '-d', help='parse data from output files in the given directory')
parser.add_argument('--redis', '-r', action='store_true', help='parse transaction data from redis')
args = parser.parse_args()

allPrevEndTimes = []
allEndTimes = []
allResults = []

if args.redis:
    r = redis.StrictRedis(host='localhost', port=6379)
    txns = r.lrange('txns', 0, -1)
    for txn in txns:
        txnDict = r.hgetall(txn)
        allPrevEndTimes.append(int(txnDict['prev-end-time']))
        allEndTimes.append(int(txnDict['end-time']))
        allResults.append(int(txnDict['committed']))

elif args.directory != None:
    resultDir = args.directory
    files = os.listdir(resultDir)

    for filename in files:
        f = open(resultDir + "/" + filename, 'r')
        for line in f:
            match = re.match("^(\d+)\s+(\d+)\s+(\d)", line)
            if match:
                prevEndTime = int(match.group(1))
                endTime = int(match.group(2))
                committed = int(match.group(3))

                allPrevEndTimes.append(prevEndTime)
                allEndTimes.append(endTime)
                allResults.append(committed)

    if len(files) == 0:
        print("No files to parse!")
        sys.exit()

print("Results span " + repr((max(allEndTimes) - min(allPrevEndTimes)) / 1000.0) + " seconds");

timeLowerBound = min(allPrevEndTimes) + (max(allEndTimes) - min(allPrevEndTimes)) * (25.0 / 100.0)
timeUpperBound = min(allPrevEndTimes) + (max(allEndTimes) - min(allPrevEndTimes)) * (75.0 / 100.0)

timeRangeSeconds = (timeUpperBound - timeLowerBound) / 1000.0

prevEndTimes = []
endTimes = []
results = []

for i in range(0, len(allPrevEndTimes)):
    if allPrevEndTimes[i] >= timeLowerBound and allEndTimes[i] <= timeUpperBound:
        prevEndTimes.append(allPrevEndTimes[i])
        endTimes.append(allEndTimes[i])
        results.append(allResults[i])

print("Selected " + repr(len(results)) + " of " + repr(len(allResults)) + " transactions over " + repr(timeRangeSeconds) + " seconds")

numTxns = 0
numAborts = 0
sumTimes = 0
times = []
for i in range(0, len(prevEndTimes)):
    numTxns += 1
    if results[i] == 1:
        time = (endTimes[i] - prevEndTimes[i])
        sumTimes += time
        times.append(time)
    else:
        numAborts += 1

sumTimesSeconds = sumTimes / 1000.0
numSuccessful = numTxns - numAborts
meanTime = float(sumTimesSeconds) / numSuccessful
meanThroughput = float(numSuccessful) / timeRangeSeconds
abortRate = float(numAborts) / numTxns

print("Num transactions: " + repr(numTxns) + ", num aborts: " + repr(numAborts));
print("Avg. latency (s): " + repr(meanTime))
print("Avg. throughput (txn/s): " + repr(meanThroughput))
print("Abort rate: " + repr(abortRate))

minLatency = min(times) / 1000.0
maxLatency = max(times) / 1000.0
print("Min latency: " + repr(minLatency))
print("Max latency: " + repr(maxLatency))
