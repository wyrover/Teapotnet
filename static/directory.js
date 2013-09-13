/*************************************************************************
 *   Copyright (C) 2011-2012 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of TeapotNet.                                     *
 *                                                                       *
 *   TeapotNet is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   TeapotNet is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with TeapotNet.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

function listDirectory(url, object, userName = '') {

	$.ajax({
		url: url,
		dataType: 'json',
		timeout: 60000
	})
	.done(function(data) {
		if(data && data.length > 0) {
			$(object).html('<table class="files"></table>');
                	var table = $(object).find('table');

			for(var i=0; i<data.length; i++) {
				var resource = data[i];
				if(!resource.url) continue;
				var link =	(resource.hash ? '/' + resource.hash.escape() : 
						(userName ? '/' + userName + (resource.contact ? '/contacts/' + resource.contact.escape() : 
						'/myself') + '/files' + (resource.url[0] != '/' ? '/' : '') + resource.url.escape() : 
						resource.name.escape())
						+ (resource.type == "directory" ? '/' : ''));

				var line = '<tr>';
				line+= '<td><a href="'+link.escape()+'">'+resource.name.escape()+'</a></td>';
				line+= '</tr>';
				table.append(line);
			}
		}
		else {
			$(object).html('No files');
		}

		$('table.files tr').css('cursor', 'pointer').click(function() {
			window.location.href = $(this).find('a').attr('href');
		});
	})
	.fail(function(jqXHR, textStatus) {
		$(object).html('Failed');
	});
}
