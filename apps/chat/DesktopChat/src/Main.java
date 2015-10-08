import java.util.List;
import java.util.Random;

import edu.washington.cs.diamond.Diamond;

public class Main {
	
	static final int MESSAGE_LIST_SIZE = 100;
	static final int NUM_ACTIONS = 1000;
	static final String MESSAGE = "Help, I'm trapped in a Diamond benchmark";
	
	static final int ACTION_READ = 0;
	static final int ACTION_WRITE = 1;
	static final String RUN_TIMED = "timed";
	static final String RUN_FIXED = "fixed";
	
	static String chatroomName = "defaultroom";
	static String userName = "defaultclient";
	static String serverName = "coldwater.cs.washington.edu";
	
	private static Diamond.DStringList messageList;
	
	public static long[] writeMessageTransaction(String msg) {
		String fullMsg = userName + ": " + msg;
		int committed = 0;
		int numAborts = 0;
		long writeTimeStart = 0;
		long writeTimeEnd = 1000 * 1000 * 1000;
		while(committed == 0) {
			writeTimeStart = System.currentTimeMillis();
			Diamond.DObject.TransactionBegin();
			messageList.Append(fullMsg);
			if (messageList.Size() > MESSAGE_LIST_SIZE) {
				messageList.Erase(0);
			}
			committed = Diamond.DObject.TransactionCommit();
			if (committed == 0) {
				numAborts++;
			}
		}
		writeTimeEnd = System.currentTimeMillis();
		long[] ret = new long[2];
		ret[0] = writeTimeEnd - writeTimeStart;
		ret[1] = numAborts;
		return ret;
	}
	
	public static long writeMessageAtomic(String msg) {
		String fullMsg = userName + ": " + msg;
		long writeTimeStart = 0;
		long writeTimeEnd = 1000 * 1000 * 1000;
		writeTimeStart = System.currentTimeMillis();
		messageList.Append(fullMsg);
		if (messageList.Size() > MESSAGE_LIST_SIZE) {
			messageList.Erase(0);
		}
		writeTimeEnd = System.currentTimeMillis();
		return (writeTimeEnd - writeTimeStart);
	}
	
	public static long[] readMessagesTransaction() {
		List<String> result = null;
		int committed = 0;
		int numAborts = 0;
		long readTimeStart = 0;
		long readTimeEnd = 1000 * 1000 * 1000;
		while (committed == 0) {
			readTimeStart = System.currentTimeMillis();
			Diamond.DObject.TransactionBegin();
			result = messageList.Members();
			committed = Diamond.DObject.TransactionCommit();
			if (committed == 0) {
				numAborts++;
			}
		}
		readTimeEnd = System.currentTimeMillis();
		long[] ret = new long[2];
		ret[0] = readTimeEnd - readTimeStart;
		ret[1] = numAborts;
		return ret;
	}
	
	public static long readMessagesAtomic() {
		List<String> result = null;
		long readTimeStart = 0;
		long readTimeEnd = 1000 * 1000 * 1000;
		readTimeStart = System.currentTimeMillis();
		result = messageList.Members();
		readTimeEnd = System.currentTimeMillis();
		return (readTimeEnd - readTimeStart);
	}
	
	public static void main(String[] args) {
		String usage = "usage: java Main run_type run_number read_fraction concurrency [client_name] [chatroom_name]\n"
					 + "    run_type: timed or fixed\n"
					 + "    run_number: the number of seconds (if timed) or the number of actions (if fixed)\n"
		 			 + "    read_fraction: decimal between 0 and 1 giving proportion of reads\n"
		 			 + "    concurrency: transaction or atomic";
		if (args.length < 4) {
			System.err.println(usage);
			System.exit(0);
		}
		String runType = args[0];
		int runNumber = Integer.parseInt(args[1]);
		double readFraction = Double.parseDouble(args[2]);
		String concurrency = args[3];
		if (args.length >= 5) {
			userName = args[4];
		}
		if (args.length >= 6) {
			chatroomName = args[5];
		}
		if (!(runType.equals(RUN_TIMED) || runType.equals(RUN_FIXED))) {
			System.err.println(usage);
			System.exit(0);
		}
		if (readFraction > 1.0 || readFraction < 0.0) {
			System.err.println(usage);
			System.exit(0);
		}
		if (!(concurrency.equals("transaction") || concurrency.equals("atomic"))) {
			System.err.println(usage);
			System.exit(0);
		}
		
		Diamond.DiamondInit(serverName);
		
		String chatLogKey = "dimessage:" + chatroomName + ":chatlog";
		
		messageList = new Diamond.DStringList();
		Diamond.DObject.Map(messageList, chatLogKey);
		
		Random rand = new Random();
		
		// Take 200 initial actions to warm up the JVM
		for (int i = 0; i < 200; i++) {
			int action = rand.nextDouble() < readFraction ? ACTION_READ : ACTION_WRITE;
			if (concurrency.equals("transaction")) {
				if (action == ACTION_READ) {
					readMessagesTransaction();
				}
				else {
					writeMessageTransaction(MESSAGE);
				}
			}
			else if (concurrency.equals("atomic")) {
				if (action == ACTION_READ) {
					readMessagesAtomic();
				}
				else {
					writeMessageAtomic(MESSAGE);
				}
			}
		}
		
		long startTime = System.currentTimeMillis();
		
		long numActions = 0;
		long totalTime = 0;
		long totalNumAborts = 0;

		while (true) {
			int action = rand.nextDouble() < readFraction ? ACTION_READ : ACTION_WRITE;
			if (concurrency.equals("transaction")) {
				if (action == ACTION_READ) {
					long[] ret = readMessagesTransaction();
					totalTime += ret[0];
					totalNumAborts += ret[1];
				}
				else {
					long[] ret = writeMessageTransaction(MESSAGE);
					totalTime += ret[0];
					totalNumAborts += ret[1];
				}
			}
			else if (concurrency.equals("atomic")) {
				if (action == ACTION_READ) {
					totalTime += readMessagesAtomic();
				}
				else {
					totalTime += writeMessageAtomic(MESSAGE);
				}
			}
			numActions++;
			long currentTime = System.currentTimeMillis();
			if (runType.equals(RUN_TIMED) && (currentTime - startTime) / 1000 > runNumber) {
				break;
			}
			if (runType.equals(RUN_FIXED) && numActions >= runNumber) {
				break;
			}
		}
		
		double averageTime = ((double)totalTime) / numActions;
		System.out.print(userName + "\t" + chatroomName + "\t" + numActions + "\t" + averageTime + "\t" + concurrency);
		if (concurrency.equals("transaction")) {
			double averageAborts = ((double)totalNumAborts) / numActions;
			System.out.print("\t" + averageAborts);
		}
		System.out.println();
	}
}