#!/usr/bin/env python38
import asyncio
import socket
import struct
import time
import websockets
from enum import Enum
import json
import requests
from collections import defaultdict

WAIT = 1
INGAME = 2
VERSUS = 1
TRAINING = 2

DEFAULTTIMEOUT = 5

SERVER = "ws://fierce-lowlands-57630.herokuapp.com/"
CONCERTO = "https://concerto-mbaacc.shib.live/l"

regions = ["NorthAmericaWest", "NorthAmericaEast","SouthAmerica","Europe","MiddleEast","Australia","Japan" ]
regionMap = { "NA West":"NorthAmericaWest", "NA East":"NorthAmericaEast",
              "SA":"SouthAmerica", "EU":"Europe",
              "Middle East":"MiddleEast", "Asia":"Japan", "OCE":"Australia" };

activeConnections = {}

def formatForServer( string ):
    fmt = string.replace( '"', '\\"' )
    return f'"{fmt}"'

class HostEntry:

    def __init__( self, name, ip ):
        self.namelen = 43
        self.name = name[:self.namelen]
        self.opponent = ""
        self.ip = ip
        self.status = WAIT
        self.numtimeouts = 0
        self.mode = VERSUS
        self.inGameText = ""
        self.jointime = time.time()

    def formatWaiting( self ):
        modestring = "UNK"
        if self.mode == VERSUS:
            modestring = "VRS"
        elif self.mode == TRAINING:
            modestring = "TRN"
        return f"{self.name:<43}|{modestring}|Waiting\x1e{self.ip}"

    def formatInGame( self ):
        if self.inGameText:
            return f"{self.inGameText}\x1e{self.ip}"
        modestring = "UNK"
        if self.mode == VERSUS:
            modestring = "VS "
        elif self.mode == TRAINING:
            modestring = "TRN"
        return f"{self.name:<self.namelen}|{modestring}|In Game\x1e{self.ip}"

    def __repr__( self ):
        if self.status == WAIT:
            return self.formatWaiting()
        elif self.status == INGAME:
            return self.formatInGame()
        else:
            return "Invalid Entry"

class Lobby:

    def __init__( self ):
        self.numEntries = 0
        self.entries = {}
        self.maxEntries = 20

    def format( self ):
        formatEntries = [ e.__repr__() for e in self.entries.values() ]
        lines= "\x1f".join( formatEntries )
        return f"{self.numEntries}\x1f{lines}"

    def addHost( self, uid, name, ipaddrport ):
        if self.numEntries < self.maxEntries:
            self.entries[ uid ] = HostEntry( name, ipaddrport )
            self.numEntries += 1
            return True
        return False

    def removeHost( self, uid ):
        if uid in self.entries:
            del self.entries[ uid ]
            self.numEntries -= 1
            return True
        return False

def requestTimeoutDecorator(func):
    def _decorator(self, *args, **kwargs):
        # access a from TestSample
        try:
            return func(self, *args, **kwargs)
        except requests.exceptions.ReadTimeout:
            print("Concerto Connection timed out")
            self.live = False
        except requests.exceptions.ReadTimeoutError:
            print("Concerto Connection timed out")
            self.live = False
        except Exception as e:
            print(f"{e!r}")
            self.live = False
    return _decorator

namelen = 46
class ConcertoClient:
    def __init__( self ):
        self.state = "Room"
        self.playerid = None
        self.roomcode = None
        self.secret = None
        self.name = None
        self.publicLobbies = {}
        self.live = True
        self.challenges = {}
        self.idle = []
        self.playing = []
        self.lock = asyncio.Lock()
        self.target = 0

    def list( self ):
        return self.publicLobbies

    @requestTimeoutDecorator
    async def update( self ):
        async with self.lock:
            if self.state == "Room":
                r = requests.get(url=CONCERTO, params={'action':'list'}, timeout=DEFAULTTIMEOUT).json()
                if 'status' in r and r['status'] == "OK":
                    self.publicLobbies = {}
                    for lobby in r['lobbies']:
                        self.publicLobbies[lobby[0]] = lobby[1]
                else:
                    self.publicLobbies = {}
                    self.live = False

            elif self.state == "Lobby":
                r = requests.get(url=CONCERTO, params={'action':'status',
                                                       'id':self.roomcode,
                                                       'secret':self.secret,
                                                       'p':self.playerid},
                                timeout=DEFAULTTIMEOUT).json()
                if 'status' in r and r['status'] == "OK":
                    self.challenges = {}
                    for c in r['challenges']:
                        self.challenges[c[1]] = (c[0], c[2])
                    self.idle = sorted( r['idle'], key=lambda x: x[1] )
                    self.playing = r['playing']
                    print(r)
                else:
                    self.live = False

    @requestTimeoutDecorator
    def create( self, name, roomtype ):
        if self.state == "Room":
            self.name = name
            r = requests.get(url=CONCERTO, params={'action':'create',
                                                   'type':roomtype,
                                                   'name':self.name},
                            timeout=DEFAULTTIMEOUT).json()
            print(r)
            if 'status' in r and r['status'] == "OK":
                self.secret = r['secret']
                self.playerid = r['msg']
                self.roomcode = r['id']
                self.state = "Lobby"
                self.challenges = {}
                for c in r['challenges']:
                    self.challenges[c[1]] = (c[0], c[2])
                self.idle = r['idle']
                self.playing = r['playing']
                return self.roomcode
            else:
                self.live = False
        else:
            self.live = False

    @requestTimeoutDecorator
    def join( self, name, code, ip ):
        self.name = name
        self.roomcode = code
        if self.state == "Room":
            r = requests.get(url=CONCERTO, params={'action':'join',
                                                   'id':code,
                                                   'name':name}, timeout=DEFAULTTIMEOUT).json()
            print(r)
            if 'status' in r and r['status'] == "OK":
                self.secret = r['secret']
                self.playerid = r['msg']
                self.ip = ip
                self.state = "Lobby"
                self.challenges = {}
                for c in r['challenges']:
                    self.challenges[c[1]] = (c[0], c[2])
                self.idle = r['idle']
                self.playing = r['playing']
                return True
            else:
                return False
        else:
            self.live = False
            return False

    @requestTimeoutDecorator
    def challenge( self, target, ip ):
        if self.state == "Lobby":
            r = requests.get(url=CONCERTO, params={'action':'challenge',
                                                   'id':self.roomcode,
                                                   'ip':ip,
                                                   't':target,
                                                   'secret':self.secret,
                                                   'p':self.playerid},
                             timeout=DEFAULTTIMEOUT).json()
            if 'status' in r and r['status'] == "OK":
                pass
            else:
                self.live = False
        else:
            self.live = False

    @requestTimeoutDecorator
    def preaccept( self ):
        r = requests.get(url=CONCERTO, params={'action':'pre_accept',
                                               'id':self.roomcode,
                                               't':self.target,
                                               'secret':self.secret,
                                               'p':self.playerid},
                         timeout=DEFAULTTIMEOUT).json()

    @requestTimeoutDecorator
    def accept( self ):
        r = requests.get(url=CONCERTO, params={'action':'accept',
                                               'id':self.roomcode,
                                               't':self.target,
                                               'secret':self.secret,
                                               'p':self.playerid},
                         timeout=DEFAULTTIMEOUT).json()

    @requestTimeoutDecorator
    def end( self ):
        r = requests.get(url=CONCERTO, params={'action':'end',
                                               'id':self.roomcode,
                                               'secret':self.secret,
                                               'p':self.playerid},
                         timeout=DEFAULTTIMEOUT).json()
        if 'status' in r and r['status'] == "OK":
            pass
        else:
            self.live = False

    def format( self ):
        if self.state == "Room":
            lines = []
            for code, players in self.publicLobbies.items():
                lines.append( f"{code}\x1e{players}" )
            flines = "\x1f".join( lines )
            return f"{len(self.publicLobbies)}\x1f{flines}"
        elif self.state == "Lobby":
            lines = []
            for entry in self.idle:
                name = entry[0]
                playerid = entry[1]
                if playerid == self.playerid:
                    continue
                statusString = "       "
                ip = "None"
                if playerid in self.challenges:
                    statusString = "HOSTING"
                    ip = self.challenges[playerid][1]
                lines.append( f"{name[:namelen]:<{namelen}}|{statusString}\x1e{ip}\x1e{playerid}" )
            for match in self.playing:
                playerstring = f"{match[0][:(namelen//2)-2]} VS {match[1][:(namelen//2)-2]}"
                players = f"{playerstring:<{namelen}}|IN GAME"
                ip = match[4]
                lines.append( f"{players}\x1e{ip}\x1e{match[2]}" )
            flines = "\x1f".join( lines[:20] )
            return f"{len(lines)}\x1f{flines}"

    def formatlobby( self ):
        lines = []
        for entry in self.idle:
            name = entry[0]
            playerid = entry[1]
            if playerid == self.playerid:
                continue
            statusString = "       "
            ip = "None"
            if playerid in self.challenges:
                statusString = "HOSTING"
                ip = self.challenges[playerid][1]
            lines.append( f"{name[:namelen]:<{namelen}}|{statusString}\x1e{ip}\x1e{playerid}" )
        for match in self.playing:
            playerstring = f"{match[0][:(namelen//2)-2]} VS {match[1][:(namelen//2)-2]}"
            players = f"{playerstring:<{namelen}}|IN GAME"
            ip = match[4]
            lines.append( f"{players}\x1e{ip}\x1e{match[2]}" )
        flines = "\x1f".join( lines[:20] )
        return f"{len(lines)}\x1f{flines}"

    def __repr__( self ):
        return f"{self.name}@{self.playerid} {self.live} {self.idle} {self.challenges}"

lobby = Lobby()

lobbylock = asyncio.Lock()
idlock = asyncio.Lock()
clientlock = asyncio.Lock()
cclients = {}

"""
lobby.addHost( 1, *dummyEntries[1] )
lobby.addHost( 2, *dummyEntries[2] )
lobby.addHost( 3, *dummyEntries[3] )
"""
# ip = 127.0.0.1:46318
#lobby.addHost( 3, "A", str(ip))
uuid = 4
print(lobby.format())

def lformat( entries ):
    dlines = []
    for v in entries.values():
        dlines.append("\x1e".join(v) )
    lines = "\x1f".join(dlines)
    return f"{len(entries)}\x1f{lines}"

def formatConf( addr, region ):
    return formatForServer( json.dumps( {"eventType":"initialConfig", "ipAddress":addr, "region":region } ) )

connectRequest = b"\x1d\x00\x01\x00\x00\x00\x01\xf6\xd7\xed\x39\xfe\x24\x03\x1e\x22\xd5\x4f\x3f\xe6\x5b\x90\x1c"
connectFinal =   b"\x1d\x00\x02\x00\x00\x00\x03\x9c\x4d\x22\x26\x1e\xcf\xd5\x20\x77\x22\x37\x9a\x64\xfd\x10\x65"
nullmsg = b''
ack1 = b'01\x00\x01\x00\x00\x00\x43\x52\xd8\x8a\x78\xaa\x39\x75\x0b\xf7\x0c\xd6\xf2\x7b\xca\xa5'
ack2 = b'01\x00\x01\x00\x00\x00\x43\x52\xd8\x8a\x78\xaa\x39\x75\x0b\xf7\x0c\xd6\xf2\x7b\xca\xa5'
versionconfig30 = b'\x1f\x00\x03\x00\x00\x00\x02\x00\x07\x00\x00\x00\x33\x2e\x30\x2e\x30\x32\x33\x0e\x00\x00\x00\x37\x30\x37\x38\x34\x36\x34\x2d\x63\x75\x73\x74\x6f\x6d\x1d\x00\x00\x00\x46\x72\x69\x2c\x20\x4a\x75\x6e\x20\x31\x31\x2c\x20\x32\x30\x32\x31\x20\x20\x31\x3a\x34\x33\x3a\x30\x35\x20\x50\x4d\x5e\x8e\x55\x45\xac\x6d\x86\xff\xd1\xc6\xd9\x1d\x6f\x32\xb4\x1c'
versionconfig = b"\x1f\x00\x00\x00\x00\x00\x02\x00\x07\x00\x00\x00\x33\x2e\x31\x2e\x30\x30\x31\x0e\x00\x00\x00\x65\x63\x32\x61\x30\x62\x34\x2d\x63\x75\x73\x74\x6f\x6d\x1d\x00\x00\x00\x53\x75\x6e\x2c\x20\x41\x75\x67\x20\x30\x31\x2c\x20\x32\x30\x32\x31\x20\x31\x32\x3a\x31\x35\x3a\x34\x39\x20\x41\x4d\xa9\x1e\x8c\x5f\x39\xb7\x65\x39\x87\x82\xe2\x4e\x28\x5a\x24\x4c"
def getShortCharaName( chara ):
    # First row
    if chara ==  22: return "Aoko"
    if chara ==   7: return "Tohno"
    if chara ==  51: return "Hime"
    if chara ==  15: return "Nanaya"
    if chara ==  28: return "Kouma"
    # Second row
    if chara ==   8: return "Miyako"
    if chara ==   2: return "Ciel"
    if chara ==   0: return "Sion"
    if chara ==  30: return "Ries"
    if chara ==  11: return "V.Sion"
    if chara ==   9: return "Wara"
    if chara ==  31: return "Roa"
    # Third row
    if chara ==   4: return "Maids"
    if chara ==   3: return "Akiha"
    if chara ==   1: return "Arc"
    if chara ==  19: return "P.Ciel"
    if chara ==  12: return "Warc"
    if chara ==  13: return "V.Akiha"
    if chara ==  14: return "M.Hisui"
    # Fourth row
    if chara ==  29: return "S.Akiha"
    if chara ==  17: return "Satsuki"
    if chara ==  18: return "Len"
    if chara ==  33: return "Ryougi"
    if chara ==  23: return "W.Len"
    if chara ==  10: return "Nero"
    if chara ==  25: return "NAC"
    # Firth row
    if chara ==  35: return "KohaMech"
    if chara ==   5: return "Hisui"
    if chara ==  20: return "Neko"
    if chara ==   6: return "Kohaku"
    if chara ==  34: return "NekoMech"
    # Last row
    if chara ==  99: return "Random"
def getMoon(moon):
    if moon == 0: return "C"
    if moon == 2: return "H"
    if moon == 1: return "F"

def unpackSpectate( data, ret ):
    type = data[0:2]
    uint8 = data[2]
    uint32 = data[3:7]
    #?
    clientMode = data[7]
    delay = data[8]
    rollback = data[9]
    wincount = data[10]
    hostplayer = data[11]
    data = data[12:]
    #names
    p1nameLen = struct.unpack('i', data[:4])[0]
    p1name = data[4:4+p1nameLen].decode( 'utf-8' )
    data = data[4+p1nameLen:]
    p2nameLen = struct.unpack('i', data[:4])[0]
    p2name = data[4:4+p2nameLen].decode( 'utf-8' )
    data = data[4+p2nameLen:]
    sessionidLen = struct.unpack('i', data[:4])[0]
    sessionid = data[4:4+sessionidLen]
    data = data[4+sessionidLen:]
    #initalgamestate
    indexedFrame = data[:8]
    stage = data[8:12]
    netplayState = data[12]
    isInTraining = data[13]
    p1Chara = data[14]
    p2Chara = data[15]
    p1Moon = data[16]
    p2Moon = data[17]
    p1Color = data[18]
    p2Color = data[19]
    data = data[20:]
    #checksum
    #print( f"{p1name} ({getMoon(p1Moon)}-{getShortCharaName(p1Chara)}) vs {p2name} ({getMoon(p2Moon)}-{getShortCharaName(p2Chara)})")

    assert len(data) == 16
    ret[ "p1name" ] = p1name
    ret[ "p1name" ] = p2name
    ret[ "p1char" ] = f"{getMoon(p1Moon)}-{getShortCharaName(p1Chara)}"
    ret[ "p2char" ] = f"{getMoon(p2Moon)}-{getShortCharaName(p2Chara)}"
    ret[ "text" ] = f"{p1name[:12]} ({getMoon(p1Moon)}-{getShortCharaName(p1Chara)}) vs {p2name[:12]} ({getMoon(p2Moon)}-{getShortCharaName(p2Chara)})"

class RelayProtocol:
    def __init__(self, on_con_lost, addr, matchid, retDict ):
        self.on_con_lost = on_con_lost
        self.transport = None
        self.addr = addr
        self.state = 1
        self.matchid = matchid
        self.retDict = retDict

    def connection_made(self, transport):
        print("conmade")
        self.transport = transport
        if self.state == 1:
            message = b"\x01" + struct.pack("l", self.matchid )
            self.transport.sendto(message, self.addr)

    def datagram_received(self, data, addr):
        print("datagram:", data)
        #print("Close the socket")
        #self.transport.close()
        if data[:3] == b'\x1d\x00\x01':
            print("UdpControl")
            val = struct.unpack('>i', data[3:7])[0]
            #print( val )
            if val == 1:
                print("ConnectRequest")
            if val == 2:
                print("ConnectReply")
                self.transport.sendto(connectFinal, self.addr)
            if val == 3:
                print("ConnectFinal")
            if val == 4:
                print("Disconnect")
        elif data[:2] == b'\x01\x00':
            print("AckSeq")
            ackId = struct.unpack( 'i', data[2:6])[0]
            print("ackid: ", ackId)
            if ackId == 1:
                self.transport.sendto(ack1, self.addr)
            if ackId == 2:
                print("sendack2")
                self.transport.sendto(data, self.addr)
        elif data[:2] == b'\x1f\x00':
            print("versionconfig")
            self.transport.sendto(versionconfig, self.addr)
            clientFlags = data[7]
            print( data )
            print( clientFlags )
            gameStarted = ( clientFlags & 0x2 ) == 0x2
            print( gameStarted )
            if gameStarted:
                self.retDict["Started"] = True
            else:
                self.retDict["Started"] = False
                self.transport.close()
        elif  data[:2] == b'\x18\x00':
            print("specteateconfig")
            unpackSpectate(data, self.retDict)
            self.transport.close()
        else:
            print("unexpected Message: ", data)
            self.transport.close()

    def error_received(self, exc):
        print('Error received:', exc)

    def connection_lost(self, exc):
        print("Connection closed")
        self.on_con_lost.set_result(True)

async def checkHostRelay( hostAddr ):
    relay = "81.4.126.110:3939"
    host, port = relay.split(":")
    reader, writer = await asyncio.open_connection(
        host, port)
    writer.write(f"T{hostAddr}".encode())
    data = await reader.read(100)
    if not data:
        print( "host down" )
        writer.close()
        return False
    matchid = struct.unpack('i', data[9:])[0]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) # UDP
    loop = asyncio.get_running_loop()
    on_con_lost = loop.create_future()
    retDict = {}
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: RelayProtocol(on_con_lost, (host, int(port)), matchid, retDict),
        sock=sock)
    data = await reader.read(200)
    matchid2 = struct.unpack('i', data[7:11])[0]
    ip = data[11:-1].decode()
    host, port = ip.split(":")
    addr =  ( host, int(port))
    protocol.addr = addr
    for _ in range(10):
        transport.sendto(connectRequest, addr)
        sock.sendto( b'', (host, int(port)))
        await asyncio.sleep(0.01)
    try:
        print("wait")
        await asyncio.wait_for( on_con_lost, timeout=5 )
    except asyncio.TimeoutError:
        print("timed out!")
        return False
    transport.close()
    writer.close()
    return retDict

async def checkHostStatus( key, name, hostAddr ):
    host, port = hostAddr.split( ":" )
    try:
        reader, writer = await asyncio.wait_for( asyncio.open_connection( host, port ), timeout=1 )
        data = await reader.read( 100 )
        clientFlags = data[9]
        print( clientFlags )
        gameStarted = ( clientFlags & 0x2 ) == 0x2
        print( gameStarted )
        if gameStarted:
            async with lobbylock:
                if key in lobby.entries:
                    lobby.entries[key].status = INGAME
        return True
    except ( asyncio.TimeoutError, ConnectionRefusedError ) as e:
        if type(e) == asyncio.TimeoutError:
            print("timed out!")
        elif type(e) == ConnectionRefusedError:
            print("connection refused" )
        print("trying relay")
        try:
            rv = await checkHostRelay( hostAddr )
            if rv:
                print(rv)
                if rv["Started"]:
                    async with lobbylock:
                        print("AAAA", rv["text"])
                        if key in lobby.entries:
                            lobby.entries[key].status = INGAME
                            lobby.entries[key].inGameText = rv[ "text" ]
                return True
        except:
            pass
        async with lobbylock:
            if key in lobby.entries:
                lobby.entries[key].numtimeouts += 1
                if lobby.entries[key].numtimeouts > 5:
                    lobby.removeHost( key )
        return False

async def updateLoop():
    while True:
        # update concerto clients
        async with clientlock:
            for client in cclients.values():
                await client.update()
        # issues with checking ips at server, just kick old ones for now
        """
        aw = []
        for k, v in lobby.entries.items():
            aw.append( checkHostStatus( k, v.name, v.ip ) )
        rv = await asyncio.gather( *aw )
        print (rv)
        await asyncio.sleep(5)
        """
        aw = {}
        for k, v in lobby.entries.items():
            if ( time.time() - v.jointime ) > 600:
                aw[k] =  time.time() - v.jointime
        print (aw)
        print (cclients)
        print ("active:", activeConnections.keys())
        active = list(activeConnections.keys())
        for c in active:
            if activeConnections[c].is_closing():
                await cleanup( activeConnections[c], c, "prune" )
        for k in aw:
            lobby.removeHost( k )
        lobby.numEntries = len(lobby.entries)
        await asyncio.sleep(5)

async def checkConnectionClosed( reader, connectionid ):
    if reader.at_eof():
        print("connection closed")
        async with lobbylock:
            lobby.removeHost( connectionid )
        return True
    return False

async def cleanup( writer, connectionid, reason="Unk" ):
    print( f"cleanup {connectionid}: {reason}")
    writer.close()
    async with clientlock:
        if connectionid in cclients:
            del cclients[connectionid]
        if connectionid in activeConnections:
            del activeConnections[connectionid]

async def handle_connect(reader, writer):
    addr = writer.get_extra_info('peername')
    print(f"Connection from {addr}")
    global lobbylock, idlock, lobby, uuid
    async with idlock:
        connectionid = uuid
        uuid += 1
        activeConnections[connectionid] = writer
        if uuid > 2147483647 - 1:
            uuid = 0
    while True:
        data = await reader.read(100)
        if await checkConnectionClosed( reader, connectionid ):
            await cleanup( writer, connectionid, "Closed" )
            return
        message = data.decode()
        try:
            req, info = message.split(",")
        except:
            print( "invalid request" )
            print( message )
            await cleanup( writer, connectionid, "invalid request" )
            return
        print( f"Req {req}@{connectionid}" )
        if req == "LIST":
            retString = "LIST\x1f"+lobby.format()
            print(f"< {retString}")
            writer.write(bytes( retString, 'utf-8'))
            await writer.drain()
        elif req == "HOST":
            name, port = info.split("|")
            if name == "":
                name = "Anonymous"
            name = name[:16]
            print("n", name)
            print("p", port)
            print("a", addr)
            async with lobbylock:
                success = lobby.addHost( connectionid, name, addr[0] + port );
            retString = f"HOST\x1f{success!r}"
            writer.write(bytes( retString, 'utf-8'))
            print("s", retString)
            await writer.drain()
        elif req == "UNHOST":
            async with lobbylock:
                lobby.removeHost( connectionid )
        elif req == "MMSTART":
            region, port = info.split("|")
            async with websockets.connect(SERVER) as websocket:
                ipaddr = addr[0]
                addrFormat = formatConf( f"{ipaddr}:{port}", regionMap[region] )
                print("Af", addrFormat)
                await websocket.send( addrFormat )
                """
                pingtest temporarily ignored
                resp = await websocket.recv()
                print(f"< {resp}")
                # ignore pingtest for now, send back dummy
                matchers = [ {"matcherID":rid,
                              "ping":str( 100 - 50 * int( rid == regionMap[ region ] ) ) } for rid in regions ]
                dummyPingTest = formatForServer( json.dumps({"eventType":'pingTestResponse',
                                                             "matchers": matchers}) )
                await websocket.send(dummyPingTest)
                """
                resp = await websocket.recv()
                print(f"< {resp}")
                jresp = json.loads( resp )
                while jresp["eventType"] == "noOpponents":
                    print(f"NOP")
                    # todo add timeout
                    resp = await websocket.recv()
                    jresp = json.loads( resp )
                    if await checkConnectionClosed( reader, connectionid ):
                        await cleanup( writer, connectionid )
                        return
                while jresp["eventType"] == "pingTest":
                    addr = jresp["address"]
                    writer.write( bytes( f"PINGTEST\x1f{addr}", 'utf-8' ) )
                    await writer.drain()
                    data = await reader.read( 100 )
                    msg = data.decode()
                    ping = int( msg )
                    msg = formatForServer( json.dumps( { "eventType" :'pingTestResponse',
                                                         "matcherID" : jresp[ "matcherID" ],
                                                         "ping" : ping  } ) )
                    await websocket.send( msg )
                    resp = await websocket.recv()
                    print(f"< {resp}")
                    jresp = json.loads( resp )
                    if await checkConnectionClosed( reader, connectionid ):
                        await cleanup( writer, connectionid )
                        return
                if jresp["eventType"] == "joinMatch":
                    msg = f"CLIENT\x1f{jresp['address']}"
                    print( msg )
                    writer.write( bytes( msg, 'utf-8' ) )
                    await writer.drain()
                elif jresp["eventType"] == "openPort":
                    print(f"HOST")
                    writer.write( bytes( "HOST", 'utf-8' ) )
                    await writer.drain()
                    data = await reader.read( 100 )
                    msg = data.decode()
                    if msg == "SUCCESS":
                        print(msg)
                        await websocket.send( formatForServer( json.dumps ( {"eventType":'portIsOpen'} ) ) )
                    await asyncio.sleep(10)
        elif req == "CLIST":
            if connectionid not in cclients:
                cclients[connectionid] = ConcertoClient();
            await cclients[connectionid].update()
            if not cclients[connectionid].live:
                await cleanup( writer, connectionid )
                return
            retString = "CLIST\x1f"+cclients[connectionid].format()
            print(f"< {retString}")
            writer.write(bytes( retString, 'utf-8'))
            await writer.drain()
        elif req == "CLOBBY":
            #print(cclients)
            if connectionid not in cclients:
                await cleanup( writer, connectionid, "missing client" )
                return
            if not cclients[connectionid].live:
                print(connectionid)
                await cleanup( writer, connectionid, "inactive client" )
                return
            async with client.lock:
                retString = "CLOBBY\x1f"+cclients[connectionid].formatlobby()
            print(f"< {retString}")
            writer.write(bytes( retString, 'utf-8'))
            await writer.drain()
        elif req == "CJOIN":
            name, code = info.split("|")
            if name == "":
                name = "Anonymous"
            name = name[:16]
            if connectionid not in cclients:
                cclients[connectionid] = ConcertoClient();
            client = cclients[connectionid]
            success = client.join( name, code, addr )
            if not success:
                if not client.live:
                    await cleanup( writer, connectionid )
                    return
                retString = "CJOINF\x1f0"
                writer.write(bytes( retString, 'utf-8'))
                await writer.drain()
            else:
                retString = "CLOBBY\x1f"+client.formatlobby()
                print(f"< {retString}")
                writer.write(bytes( retString, 'utf-8'))
                await writer.drain()
        elif req == "CCHAL":
            target, port = info.split("|")
            if connectionid not in cclients:
                await cleanup( writer, connectionid )
                return
            client = cclients[connectionid]
            ipaddr = addr[0]
            client.challenge( target, f"{ipaddr}:{port}" )
            if not client.live:
                await cleanup( writer, connectionid )
                return
            retString = "CCHAL\x1fOK"
            print(f"< {retString}")
            writer.write(bytes( retString, 'utf-8'))
            await writer.drain()
        elif req == "CCREATE":
            name, lobbytype = info.split("|")
            if name == "":
                name = "Anonymous"
            name = name[:16]
            if connectionid not in cclients:
                cclients[connectionid] = ConcertoClient();
            client = cclients[connectionid]
            code = client.create( name, lobbytype )
            if not client.live:
                await cleanup( writer, connectionid )
                return
            async with client.lock:
                retString = f"CCREATE\x1f"+client.formatlobby()+f"\x1f{code}"
            print(f"< {retString}")
            writer.write(bytes( retString, 'utf-8'))
            await writer.drain()
        elif req == "CPREACCEPT":
            target = info
            if connectionid not in cclients:
                await cleanup( writer, connectionid, "client does not exist" )
                return
            client = cclients[connectionid]
            client.target = target
            client.preaccept()
        elif req == "CACCEPT":
            if connectionid not in cclients:
                await cleanup( writer, connectionid, "client does not exist" )
                return
            client = cclients[connectionid]
            client.accept()
        elif req == "CEND":
            if connectionid not in cclients:
                pass
            cclients[connectionid].end()
            if not client.live:
                await cleanup( writer, connectionid )
                return
        else:
            print("invalid request")
            print(data)
            print("connection closed")
            if connectionid in cclients:
                del cclients[connectionid]
            writer.close()
            return


        await asyncio.sleep(1)

    print("Close the connection")
    if connectionid in cclients:
        del cclients[connectionid]
    writer.close()

async def main():
    server = await asyncio.start_server(
        handle_connect, '0.0.0.0', 3502 )

    addr = server.sockets[0].getsockname()
    print(f'Serving on {addr}')

    task = asyncio.create_task( updateLoop() )
    await task
    async with server:
        await server.serve_forever()

asyncio.run(main())
