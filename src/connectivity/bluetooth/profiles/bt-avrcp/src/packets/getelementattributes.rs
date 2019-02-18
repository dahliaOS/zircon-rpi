// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;

#[derive(Debug)]
pub struct GetElementAttributesCommand {
    attributes: Vec<MediaAttributeId>,
}

impl GetElementAttributesCommand {
    pub fn all_attributes() -> GetElementAttributesCommand {
        Self { attributes: MediaAttributeId::VARIANTS.to_vec() }
    }

    #[allow(dead_code)]
    pub fn from_attributes(attributes: &[MediaAttributeId]) -> GetElementAttributesCommand {
        if attributes.len() == 0 {
            return Self::all_attributes();
        }
        Self { attributes: attributes.to_vec() }
    }

    #[allow(dead_code)]
    pub fn attributes(&self) -> &[MediaAttributeId] {
        return &self.attributes[..];
    }
}

impl VendorDependent for GetElementAttributesCommand {
    fn pdu_id(&self) -> PduId {
        PduId::GetElementAttributes
    }
}

impl Decodable for GetElementAttributesCommand {
    fn decode(buf: &[u8]) -> PacketResult<Self> {
        if buf.len() < 9 {
            return Err(Error::OutOfRange);
        }
        let identifier = &buf[0..8];
        if identifier != &[0; 8] {
            // Only supported command is NOW_PLAYING (0x00 x8)
            return Err(Error::UnsupportedMessage);
        }
        let attribute_count = buf[8] as usize;

        let mut attributes;
        if attribute_count == 0 {
            attributes = MediaAttributeId::VARIANTS.to_vec();
        } else {
            if buf.len() < 9 + attribute_count {
                return Err(Error::InvalidMessage);
            }
            attributes = vec![];
            for i in (12..(attribute_count * 4 + 12)).step_by(4) {
                attributes.push(MediaAttributeId::try_from(buf[i])?);
            }
        }

        Ok(Self { attributes })
    }
}

impl Encodable for GetElementAttributesCommand {
    fn encoded_len(&self) -> usize {
        let mut len = 9;
        if self.attributes != MediaAttributeId::VARIANTS {
            len += self.attributes.len() * 4
        }
        len
    }

    fn encode(&self, buf: &mut [u8]) -> PacketResult<()> {
        if buf.len() < self.encoded_len() {
            return Err(Error::OutOfRange);
        }

        // Only supported command is NOW_PLAYING (0x00 x8)
        buf[0..8].fill(0);
        if self.attributes == MediaAttributeId::VARIANTS {
            buf[8] = 0;
        } else {
            buf[8] = u8::try_from(self.attributes.len()).map_err(|_| Error::OutOfRange)?;
            let mut i = 12;
            for attr in self.attributes.iter() {
                buf[i] = u8::from(attr);
                i += 4;
            }
        }

        Ok(())
    }
}

#[derive(Debug)]
pub struct GetElementAttributesResponse {
    pub title: Option<String>,
    pub artist_name: Option<String>,
    pub album_name: Option<String>,
    pub track_number: Option<String>,
    pub total_number_of_tracks: Option<String>,
    pub genre: Option<String>,
    pub playing_time: Option<String>,
    pub default_cover_art: Option<String>, // BIP handle encoded as string
}

impl Default for GetElementAttributesResponse {
    fn default() -> Self {
        Self {
            title: None,
            artist_name: None,
            album_name: None,
            track_number: None,
            total_number_of_tracks: None,
            genre: None,
            playing_time: None,
            default_cover_art: None,
        }
    }
}

impl VendorDependent for GetElementAttributesResponse {
    fn pdu_id(&self) -> PduId {
        PduId::GetElementAttributes
    }
}

impl Decodable for GetElementAttributesResponse {
    fn decode(buf: &[u8]) -> PacketResult<Self> {
        if buf.len() < 1 {
            return Err(Error::OutOfRange);
        }

        let mut response = Self::default();
        let _attribute_count = buf[0];
        let mut offset = 1;
        loop {
            let attribute_header = buf.get(offset..offset + 8).ok_or(Error::OutOfRange)?;

            let attribute = MediaAttributeId::try_from(attribute_header[3])?;
            let _charset_id = ((attribute_header[4] as u16) << 8) | (attribute_header[5] as u16);
            let attribute_len =
                (((attribute_header[6] as u16) << 8) | (attribute_header[7] as u16)) as usize;
            offset += 8;

            if attribute_len > 0 {
                let attribute_value =
                    buf.get(offset..offset + attribute_len).ok_or(Error::OutOfRange)?;

                // Todo: validate charset_id is UTF8 or ASCII
                let attribute_string = String::from_utf8_lossy(attribute_value).to_string();

                match attribute {
                    MediaAttributeId::Title => response.title = Some(attribute_string),
                    MediaAttributeId::ArtistName => response.artist_name = Some(attribute_string),
                    MediaAttributeId::AlbumName => response.album_name = Some(attribute_string),
                    MediaAttributeId::TrackNumber => response.track_number = Some(attribute_string),
                    MediaAttributeId::TotalNumberOfTracks => {
                        response.total_number_of_tracks = Some(attribute_string)
                    }
                    MediaAttributeId::Genre => response.genre = Some(attribute_string),
                    MediaAttributeId::PlayingTime => response.playing_time = Some(attribute_string),
                    MediaAttributeId::DefaultCoverArt => {
                        response.default_cover_art = Some(attribute_string)
                    }
                }
                offset += attribute_len;
            }

            if offset == buf.len() {
                break;
            } else if offset > buf.len() {
                return Err(Error::InvalidMessage);
            }
        }
        Ok(response)
    }
}

impl Encodable for GetElementAttributesResponse {
    fn encoded_len(&self) -> usize {
        let mut len = 1;
        let mut count = |os: &Option<String>| {
            if let Some(ref s) = os {
                len += s.len() + 8;
            }
        };
        count(&self.title);
        count(&self.artist_name);
        count(&self.album_name);
        count(&self.track_number);
        count(&self.total_number_of_tracks);
        count(&self.genre);
        count(&self.playing_time);
        count(&self.default_cover_art);
        len
    }

    fn encode(&self, buf: &mut [u8]) -> PacketResult<()> {
        if buf.len() < self.encoded_len() {
            return Err(Error::OutOfRange);
        }

        let mut attribute_count = 0;
        let mut offset = 1;

        let mut write = |os: &Option<String>, attribute_id: MediaAttributeId| -> PacketResult<()> {
            if let Some(ref s) = os {
                attribute_count += 1;
                let attribute_header = buf.get_mut(offset..offset + 8).ok_or(Error::OutOfRange)?;

                let strlen = u16::try_from(s.len()).unwrap_or(std::u16::MAX) as usize;
                attribute_header[3] = u8::from(&attribute_id);
                attribute_header[4] = 0;
                attribute_header[5] = 106; // UTF-8 MIBEnum
                attribute_header[6] = (strlen >> 8) as u8;
                attribute_header[7] = (strlen & 0xff) as u8;
                offset += 8;
                buf[offset..offset + strlen].copy_from_slice(&s.as_bytes()[..strlen]);
                offset += strlen;
            }
            Ok(())
        };
        write(&self.title, MediaAttributeId::Title)?;
        write(&self.artist_name, MediaAttributeId::ArtistName)?;
        write(&self.album_name, MediaAttributeId::AlbumName)?;
        write(&self.track_number, MediaAttributeId::TrackNumber)?;
        write(&self.total_number_of_tracks, MediaAttributeId::TotalNumberOfTracks)?;
        write(&self.genre, MediaAttributeId::Genre)?;
        write(&self.playing_time, MediaAttributeId::PlayingTime)?;
        write(&self.default_cover_art, MediaAttributeId::DefaultCoverArt)?;

        buf[0] = attribute_count;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_element_attributes_command_encode_all_attributes() {
        let b = GetElementAttributesCommand::all_attributes();
        assert_eq!(
            b.attributes(),
            &[
                MediaAttributeId::Title,
                MediaAttributeId::ArtistName,
                MediaAttributeId::AlbumName,
                MediaAttributeId::TrackNumber,
                MediaAttributeId::TotalNumberOfTracks,
                MediaAttributeId::Genre,
                MediaAttributeId::PlayingTime,
                MediaAttributeId::DefaultCoverArt
            ]
        );
        assert_eq!(b.pdu_id(), PduId::GetElementAttributes);
        assert_eq!(b.encoded_len(), 9); // identifier, length
        let mut buf = vec![0; b.encoded_len()];
        assert!(b.encode(&mut buf[..]).is_ok());
        assert_eq!(
            buf,
            &[
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // NOW_PLAYING identifier
                0x00  // passing 0 for all attributes
            ]
        );
    }

    #[test]
    fn test_get_element_attributes_command_encode_some_attributes() {
        let b = GetElementAttributesCommand::from_attributes(&[
            MediaAttributeId::Title,
            MediaAttributeId::ArtistName,
        ]);
        assert_eq!(b.pdu_id(), PduId::GetElementAttributes);
        assert_eq!(b.attributes(), &[MediaAttributeId::Title, MediaAttributeId::ArtistName]);
        assert_eq!(b.encoded_len(), 8 + 1 + 4 + 4); // identifier, length, 2 attributes
        let mut buf = vec![0; b.encoded_len()];
        assert!(b.encode(&mut buf[..]).is_ok());
        assert_eq!(
            buf,
            &[
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // NOW_PLAYING identifier
                0x02, // 2 attributes
                0x00, 0x00, 0x00, 0x01, // Title
                0x00, 0x00, 0x00, 0x02, // ArtistName
            ]
        );
    }

    #[test]
    fn test_get_element_attributes_command_decode_some_attributes() {
        let b = GetElementAttributesCommand::decode(&[
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // NOW_PLAYING identifier
            0x02, // 2 attributes
            0x00, 0x00, 0x00, 0x01, // Title
            0x00, 0x00, 0x00, 0x02, // ArtistName
        ])
        .expect("unable to decode");
        assert_eq!(b.attributes(), &[MediaAttributeId::Title, MediaAttributeId::ArtistName]);
        assert_eq!(b.encoded_len(), 8 + 1 + 4 + 4); // identifier, length, 2 attributes
    }

    #[test]
    fn test_get_element_attributes_command_decode_all_attributes() {
        let b = GetElementAttributesCommand::decode(&[
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // NOW_PLAYING identifier
            0x00, // 0 attributes for all
        ])
        .expect("unable to decode");
        assert_eq!(b.pdu_id(), PduId::GetElementAttributes);
        assert_eq!(
            b.attributes(),
            &[
                MediaAttributeId::Title,
                MediaAttributeId::ArtistName,
                MediaAttributeId::AlbumName,
                MediaAttributeId::TrackNumber,
                MediaAttributeId::TotalNumberOfTracks,
                MediaAttributeId::Genre,
                MediaAttributeId::PlayingTime,
                MediaAttributeId::DefaultCoverArt
            ]
        );
        assert_eq!(b.encoded_len(), 8 + 1); // identifier, attribute count
    }

    #[test]
    fn test_get_element_attributes_response_encode() {
        let b = GetElementAttributesResponse {
            title: Some(String::from("Test")),
            ..GetElementAttributesResponse::default()
        };
        assert_eq!(b.pdu_id(), PduId::GetElementAttributes);
        assert_eq!(b.encoded_len(), 1 + 8 + 4); // count, attribute header (8), attribute encoded len (len of "Test")
        let mut buf = vec![0; b.encoded_len()];
        assert!(b.encode(&mut buf[..]).is_ok());
        assert_eq!(
            buf,
            &[
                0x01, // count
                0x00, 0x00, 0x00, 0x01, // element attribute id (title)
                0x00, 0x6a, // encoding UTF-8
                0x00, 0x04, // attribute length
                'T' as u8, 'e' as u8, 's' as u8, 't' as u8, // attribute payload
            ]
        );
    }

    #[test]
    fn test_get_element_attributes_response_decode() {
        let b = GetElementAttributesResponse::decode(&[
            0x01, // count
            0x00, 0x00, 0x00, 0x01, // element attribute id (title)
            0x00, 0x6a, // encoding UTF-8
            0x00, 0x04, // attribute length
            'T' as u8, 'e' as u8, 's' as u8, 't' as u8, // attribute payload
        ])
        .expect("unable to decode packet");
        assert_eq!(b.pdu_id(), PduId::GetElementAttributes);
        assert_eq!(b.encoded_len(), 1 + 8 + 4); // count, attribute header (8), attribute encoded len (len of "Test")
        assert_eq!(b.title, Some(String::from("Test")));
    }

    #[test]
    fn test_encode_packets() {
        let b = GetElementAttributesResponse {
            title: Some(String::from(
                "Lorem ipsum dolor sit amet,\
                 consectetur adipiscing elit. Nunc eget elit cursus ipsum \
                 fermentum viverra id vitae lorem. Cras luctus elementum \
                 metus vel volutpat. Vestibulum ante ipsum primis in \
                 faucibus orci luctus et ultrices posuere cubilia \
                 Curae; Praesent efficitur velit sed metus luctus",
            )),
            artist_name: Some(String::from(
                "elit euismod. \
                 Sed ex mauris, convallis a augue ac, hendrerit \
                 blandit mauris. Integer porttitor velit et posuere pharetra. \
                 Nullam ultricies justo sit amet lorem laoreet, id porta elit \
                 gravida. Suspendisse sed lectus eu lacus efficitur finibus. \
                 Sed egestas pretium urna eu pellentesque. In fringilla nisl dolor, \
                 sit amet luctus purus sagittis et. Mauris diam turpis, luctus et pretium nec, \
                 aliquam sed odio. Nulla finibus, orci a lacinia sagittis,\
                 urna elit ultricies dolor, et condimentum magna mi vitae sapien. \
                 Suspendisse potenti. Vestibulum ante ipsum primis in faucibus orci \
                 luctus et ultrices posuere cubilia Curae",
            )),
            album_name: Some(String::from(
                "Mauris in ante ultrices, vehicula lorem non, sagittis metus.\
                 Nam facilisis volutpat quam. Suspendisse sem ipsum, blandit ut faucibus vitae,\
                 facilisis quis massa. Aliquam sagittis, orci sed dignissim vulputate, odio neque \
                 tempor dui, vel feugiat metus massa id urna. Nam at risus sem.\
                 Duis commodo suscipit metus, at placerat elit suscipit eget. Suspendisse interdum \
                 id metus vitae porta. Ut cursus viverra imperdiet. Aliquam erat volutpat. \
                 Curabitur vehicula mauris nec ex sollicitudin rhoncus. Integer ipsum libero, \
                 porta id velit et, egestas facilisis tellus.",
            )),
            genre: Some(String::from(
                "Mauris in ante ultrices, vehicula lorem non, sagittis metus.\
                 Nam facilisis volutpat quam. Suspendisse sem ipsum, blandit ut faucibus vitae,\
                 facilisis quis massa. Aliquam sagittis, orci sed dignissim vulputate, odio neque \
                 tempor dui, vel feugiat metus massa id urna. Nam at risus sem.\
                 Duis commodo suscipit metus, at placerat elit suscipit eget. Suspendisse interdum \
                 id metus vitae porta. Ut cursus viverra imperdiet. Aliquam erat volutpat. \
                 Curabitur vehicula mauris nec ex sollicitudin rhoncus. Integer ipsum libero, \
                 porta id velit et, egestas facilisis tellus.",
            )),
            ..GetElementAttributesResponse::default()
        };
        let packets = b.encode_packets().expect("unable to encode packets for event");
        println!(
            "count: {}, len of 0: {}, packets: {:x?}",
            packets.len(),
            packets[0].len(),
            packets
        );
        assert!(packets.len() > 2);
        // each packet should be the get element attributes PDU type
        assert_eq!(packets[0][0], 0x20);
        assert_eq!(packets[1][0], 0x20);

        assert_eq!(packets[0][1], 0b01); // first packet should be a start packet.
        assert_eq!(packets[1][1], 0b10); // second packet should be a continue packet.
        assert_eq!(packets[packets.len() - 1][1], 0b11); // last packet should be a stop packet.

        for p in packets {
            assert!(p.len() <= 512);
        }
    }
}
