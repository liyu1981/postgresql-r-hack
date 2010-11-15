#!/usr/bin/twistd -ny
#
#-------------------------------------------------------------------------
#
#	  Emulated Group Communication System
#
# WARNING: this is not a real group communication system (GCS), so
#		   please do not use this for anything serious.
#
# This simple TCP server mainly  serves as a  simple communication system
# to test Postgres-R. It runs on one single node and expects all 'clients'
# to connect to the same server. It features joining and leaving of
# groups as well as sending totally ordered messages (ordered by arrival
# on the single node server).
#
# It requires the twisted framework (http://twistedmatrix.com/) and should
# be started by: 'twistd -ny ecgs.tac'.
#
# Copyright (c) 2006-2010, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

# the port to listen on
PORT = 54320

# switch for verbose logging
VERBOSE = False

#-------------------------------------------------------------------------

import re, os, sys, struct
from twisted.internet import protocol
from twisted.application import internet, service

class EGCS(protocol.Protocol):

	def __init__(self, factory, node_id):
		self.factory = factory
		self.node_id = node_id
		self.disconnected = 0
		self.buffer = ''

	def getNodeId(self):
		return self.node_id

	def __repr__(self):
		return "node %d" % self.node_id

	def connectionMade(self):
		pass

	def connectionLost(self, reason):
		self.disconnected = 1
		self.factory.removeNode(self, reason)

	def dataReceived(self, data):
		self.buffer += data
		while self.processMessage():
			pass

	def processMessage(self):
		""" Tries to process a message from the buffer, returns true on
			success or false if there's no complete message in the buffer.
		"""
		data = self.buffer
		if len(data) < 2:		 # no message type has less than 2 bytes
			return False

		if data[0] == 'H':
			# a hello message.
			proto_version = struct.unpack('!B', data[1])[0]

			if proto_version == 1:
				data = ''
				raise Exception(
					"EGCS Protocol Version 1 no longer supported.")
 
			elif proto_version == 2:
				msg = struct.pack('!cBi', 'H', 2, self.getNodeId())
				self.transport.write(msg)

				self.buffer = self.buffer[2:]
				return True

			else:
				raise Exception("Invalid protocol version")

		elif data[0] == 'J':
			strlen = struct.unpack('!B', data[1])[0]

			if len(data) < strlen + 2:
				return False

			group_name = struct.unpack('!%ds' % strlen, data[2:2+strlen])[0]

			self.factory.join(self, group_name)

			self.buffer = self.buffer[strlen+2:]
			return True

		elif data[0] == 'L':
			strlen = struct.unpack('!B', data[1])[0]

			if len(data) < strlen + 2:
				return False

			group_name = struct.unpack('!%ds' % strlen, data[2:2+strlen])[0]

			self.factory.leave(self, group_name)

			self.buffer = self.buffer[strlen+2:]
			return True

		elif data[0] == 'M':
			if len(data) < 6:
				return False

			node_id = struct.unpack('!i', data[1:5])[0]

			if node_id != self.node_id:
				raise Exception("%s cannot send message for node %d" \
					% (self, node_id))

			strlen = struct.unpack('!B', data[5])[0]

			if len(data) < 6  + strlen + 4:
				return False

			group_name = struct.unpack('!%ds' % strlen, data[6:6+strlen])[0]

			msg_size = struct.unpack('!i', data[6+strlen:6+strlen+4])[0]

			if len(data) < 6 + strlen + 4 + msg_size:
				return False

			msg_data = data[6+strlen+4:6+strlen+4+msg_size]
			assert len(msg_data) == msg_size

			self.factory.sendGroupMsg(node_id, group_name, msg_data)

			self.buffer = self.buffer[6+strlen+4+msg_size:]
			return True

		elif data[0] == 'm':
			if len(data) < 9:
				return False

			sender_node_id = struct.unpack('!i', data[1:5])[0]
			recp_node_id = struct.unpack('!i', data[5:9])[0]

			if sender_node_id != self.node_id:
				raise Exception("%s cannot send messagen for node %d" \
					% (self, sender_node_id))

			strlen = struct.unpack('!B', data[9])[0]

			if len(data) < 9 + strlen + 4:
				return False

			group_name = struct.unpack('!%ds' % strlen, data[10:10+strlen])[0]

			msg_size = struct.unpack('!i', data[10+strlen:10+strlen+4])[0]

			if len(data) < 9 + strlen + 4 + msg_size:
				return False

			msg_data = data[10+strlen+4:10+strlen+4+msg_size]
			assert(len(msg_data) == msg_size)

			self.factory.sendNodeMsg(sender_node_id, group_name, recp_node_id, msg_data)

			self.buffer = self.buffer[10+strlen+4+msg_size:]
			return True

		else:
			raise Exception("EGCS cannot handle message type '%s' (%d)" % \
				(data[0], ord(data[0])) +
				"from node id %d" % (self.node_id,))
			self.transport.loseConnection()
			data = ''

class EGCSFactory(protocol.Factory):

	def __init__(self, reporter):
		self.last_node_id = 0
		self.nodes = []
		self.groups = {}
		self.reporter = reporter

	def getNodeById(self, node_id):
		for node in self.nodes:
			if node.getNodeId() == node_id:
				return node
		raise KeyError, "no node with id %d" % node_id

	def sendNode(self, node, msg):
		if not node.disconnected:
			node.transport.write(msg)

	def sendGroup(self, group, msg):
		for node in group:
			self.sendNode(node, msg)

	def reportError(self, msg):
		self.reporter.reportError(msg)

	def reportInfo(self, msg):
		self.reporter.reportInfo(msg)

	def sendGroupMsg(self, node_id, group_name, msg):
		group = self.groups[group_name]
		node = None
		try:
			node = self.getNodeById(node_id)
		except KeyError, e:
			self.reportError("ignoring message from unknown node %d" \
							 % node_id)
			return

		self.reportInfo("%s sends a message to group %s of size %d" \
						% (node, group_name, len(msg)))

		msg = struct.pack('!ciB%dsi%ds' % (len(group_name), len(msg)),
			'M', node.getNodeId(), len(group_name), group_name,
			len(msg), msg)

		self.sendGroup(group, msg)

	def sendNodeMsg(self, from_node_id, group_name, to_node_id, msg):
		from_node = to_node = None
		try:
			from_node = self.getNodeById(from_node_id)
		except KeyError, e:
			self.reporter.reportError("ignoring message from unknown node %d" % ( \
				from_node_id))

		try:
			to_node = self.getNodeById(to_node_id)
		except KeyError, e:
			self.reporter.reportError("ignoring message from %d to unknown node %d" % ( \
				from_node_id, to_node_id))

		self.reporter.reportInfo("%s sends a message of size %d to %s" % ( \
			from_node, len(msg), to_node))

		msg = struct.pack('!ciiB%dsi%ds' % (len(group_name), len(msg)),
			'm', from_node.getNodeId(), to_node.getNodeId(),
			len(group_name), group_name, len(msg), msg)

		self.sendNode(to_node, msg)

	def join(self, node, group_name):
		self.reporter.reportInfo("%s joins group %s" % (node, group_name))

		if not self.groups.has_key(group_name):
			self.groups[group_name] = []

		group = self.groups[group_name]

		# send the initial view of the group
		for n in group:
			msg = struct.pack('!cciB%ds' % len(group_name), 'V', 'I',
							  n.getNodeId(), len(group_name), group_name)
			self.sendNode(node, msg)

		group.append(node)

		# send a viewchange message
		msg = struct.pack('!cciB%ds' % len(group_name), 'V', 'J',
						  node.getNodeId(), len(group_name), group_name)
		self.sendGroup(group, msg)

	def leave(self, node, group_name):
		self.reporter.reportInfo("%s leaves group %s" % (node, group_name))

		# get the group in quesion
		if not self.groups.has_key(group_name):
			raise ValueError, "%s is not in group %s" % (node, group_name)
		group = self.groups[group_name]

		# send a viewchange message
		msg = struct.pack('!cciB%ds' % len(group_name), 'V', 'L',
						  node.getNodeId(), len(group_name), group_name)
		self.sendGroup(group, msg)

		# remove the node
		group.remove(node)

	def removeNode(self, node, reason):
		self.reporter.reportInfo("%s has disconnected: %s" % ( \
			node, reason.getErrorMessage()))

		for group_name, group in self.groups.iteritems():
			if node in group:
				self.leave(node, group_name)

		self.nodes.remove(node)

	def buildProtocol(self, addr):
		self.last_node_id += 1
		proto = EGCS(self, self.last_node_id)
		self.nodes.append(proto)
		return proto

	def stopFactory(self):
		self.reporter.reportInfo("shutdown: notifying connected nodes")

		for group_name, group in self.groups.iteritems():
			for node in group:
				self.leave(node, group_name)

		for node in self.nodes:
			node.transport.loseConnection()

		protocol.Factory.stopFactory(self)

class ConsoleReporter:

	def reportInfo(self, msg):
		print "INFO:    %s" % msg

	def reportError(self, msg):
		print "WARNING: %s" % msg

factory = EGCSFactory(ConsoleReporter())

application = service.Application("egcs")
egcsService = internet.TCPServer(PORT, factory)
egcsService.setServiceParent(application)

