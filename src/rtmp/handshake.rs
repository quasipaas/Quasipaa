// use.
use bytes::BytesMut;
use crate::util::rand_numbers;


/// # Handshake Info.
pub struct Handshake {
    pub version: u8, // version
    pub completed: bool,  // is ok
    pub timestamp: Vec<u8> // timestamp
}


impl Handshake {

    /// # Creatd Handshake.
    /// 
    pub fn new () -> Self {
        Handshake { 
            version: 0, 
            completed: false,
            timestamp: vec![]
        }
    }

    /// # Examination Handshake Package.
    /// 
    pub fn then (&mut self, bytes: BytesMut) -> (bool, bool) {
        let mut is_type = false;
        let mut is_back = false;
        let mut index = 0;

        // examination package length.
        // C0 + C1
        if bytes.len() == 1537 {
            // C0, S0
            // lock version number is 3
            if bytes[0] == 3 {
                index = 5;
                is_back = true;
            }
            // parse timestamp.
            let bytes_vec = &bytes.to_vec();
            let (left, _) = bytes_vec.split_at(5);
            let (_, right) = left.split_at(1);
            self.timestamp = right.to_vec();
        } else {
            index = 4;
        }

        // C1, C2
        // S1, S2
        // TODO: check only the default placeholder.
        if index > 0 {
            if bytes[index] == 0 
            && bytes[index + 1] == 0 
            && bytes[index + 2] == 0 
            && bytes[index + 3] == 0 {
                is_type = true
            }
        }

        // callback type and back.
        (is_type, is_back)
    }

    /// # Create Handshake Package.
    /// S0 + S1 + S2
    /// 
    pub fn created (&self) -> BytesMut {
        let mut package = vec![];

        // get cache time.
        let timestamp = match self.timestamp.len() {
            0 => vec![0, 0, 0, 0],
            _ => self.timestamp.clone()
        };
        
        // push bytes.
        package.extend_from_slice(&vec![3]); // S0
        package.extend_from_slice(&timestamp); // S1 timestamp
        package.extend_from_slice(&vec![0; 4]); // S1 zero
        package.extend_from_slice(&rand_numbers(1528)); // S1 body
        package.extend_from_slice(&timestamp); // S2 timestamp
        package.extend_from_slice(&vec![0; 4]); // S2 zero
        package.extend_from_slice(&rand_numbers(1528)); // S2 body
        BytesMut::from(package)
    }

    /// # Drop Handshake Package.
    /// 
    pub fn drop (&mut self, bytes: &BytesMut) -> BytesMut {
        let mut back: Vec<u8> = vec![];
        let mut is_bool = false;
        let mut is_type = false;
        
        // check length.
        if bytes.len() >= 1536 {
            let byt_cp = bytes.clone();
            let (left, _) = byt_cp.split_at(1536);
            let (types, _) = self.then(BytesMut::from(left));
            is_type = types;
        }

        // check is handshake package.
        if is_type == true {
            let (_, right) = &bytes.split_at(1536);
            back = Vec::from(*right);
            is_bool = true;
        }

        // check is split.
        match is_bool {
            true => BytesMut::from(back),
            false => bytes.clone()
        }
    }

    /// # Check if need to handle the handshake.
    /// 
    pub fn metch (&mut self, bytes: &BytesMut) -> (BytesMut, bool) {
        let (is_type, is_back) = self.then(bytes.clone());
        let mut back = BytesMut::new();

        // need callback handshake.
        if is_type == true && is_back == true {
            back = self.created();
        }

        // not callback handshake.
        // drop handshake package.
        if is_type == true && is_back == false {
            back = self.drop(bytes);
            self.completed = true;
        }

        (back, is_back)
    }
}